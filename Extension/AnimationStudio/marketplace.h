#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
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

// THE CREW: the testers who shaped the Animator Studio, by their Thunderstore team. Their packs are marked CREW
// CERTIFIED, come first in the shop's own picks, and each of them has a page of their own in the MARKET. Kept here, in
// the build and not in a file, so nobody can make themselves crew: a pack's team is the one Thunderstore published it
// under, which only that team's members can publish to. A new tester comes in with a new build. (Each team was read
// off the tester's own service key with Thunderstore's current-user call; the keys themselves are not kept anywhere.)
struct CrewMember {
    std::string_view team, name, about;
};
inline constexpr std::array crew_members{
    CrewMember{"leafbro", "leaf.bro", "Tested the Animator Studio before it went out, so your tricks land the way you keyed them."},
    CrewMember{"Myawsometeam", "Setsu", "Tested the Animator Studio before it went out, so your tricks land the way you keyed them."},
    CrewMember{"FIENDSKATEBOARDS", "renzoh.", "Tested the Animator Studio before it went out, so your tricks land the way you keyed them."},
    CrewMember{"smonky", "smonk", "Tested the Animator Studio before it went out, so your tricks land the way you keyed them."},
    CrewMember{"reskate_animation_editor", "philsteezy", "Tested the Animator Studio before it went out, so your tricks land the way you keyed them."},
    CrewMember{"Heatz", "Heatz", "Tested the Animator Studio before it went out, so your tricks land the way you keyed them."},
};
// Sample packs, so the shop can be seen full before it is: shown after the real ones, with covers made by the cover
// designer, and nothing to GET. Off: the release shows the real packs only.
inline constexpr bool show_samples = false;
struct SamplePack {
    std::string_view title, owner;
    int tricks;
    std::string_view stance;
    int downloads, likes, days;
    std::string_view base, banner, font, place; // its cover (studio_cover_maker.h)
};
inline constexpr std::array samples{
    SamplePack{"Street Essentials", "Fourth_Studio", 24, "ALL STANCES", 2410, 312, 3, "skater", "blue", "anton", "bottom"},
    SamplePack{"Heavy Hitters", "22SH", 28, "ALL STANCES", 1480, 204, 6, "grip", "white", "dirt", "middle"},
    SamplePack{"Smooth Operator", "Norms", 20, "REGULAR", 1120, 160, 19, "concrete", "grip", "knewave", "bottom"},
    SamplePack{"Tech Deck", "Steez", 32, "GOOFY", 940, 131, 44, "blue", "white", "sedgwick", "middle"},
    SamplePack{"After Hours", "Fourth_Studio", 26, "ALL STANCES", 1390, 188, 2, "print", "blue", "bebas", "top"},
    SamplePack{"Ledge Lords", "kurt_ollie", 18, "REGULAR", 2010, 266, 5, "grip", "blue", "anton", "middle"},
    SamplePack{"Night Session", "dannyb", 14, "SWITCH", 1660, 221, 1, "print", "none", "knewave", "bottom"},
    SamplePack{"Manual Labor", "pushmob", 22, "FAKIE", 860, 97, 12, "paper", "grip", "anton", "middle"},
    SamplePack{"Flip Theory", "tre_flip_tom", 30, "ALL STANCES", 4890, 640, 71, "concrete", "tape", "bebas", "top"},
    SamplePack{"Gap Season", "saltwater_sk8", 12, "NOLLIE", 610, 64, 9, "blue", "grip", "dirt", "bottom"},
    SamplePack{"Curb Appeal", "mrs_grind", 16, "GOOFY", 3720, 512, 103, "skater", "white", "sedgwick", "top"},
    SamplePack{"Stair Masters", "bigspin_bee", 20, "ALL STANCES", 3180, 455, 58, "grip", "tape", "knewave", "middle"},
    SamplePack{"Backyard Bowl", "pool_boy", 9, "REGULAR", 230, 31, 0, "paper", "blue", "sedgwick", "middle"},
    SamplePack{"Late Flips Only", "lateflip_lou", 11, "SWITCH", 150, 22, 0, "concrete", "white", "anton", "top"},
};
// Packs the shop puts up front (FEATURED), by Team-Name, in this order. With none, or none of them out, it picks: the
// crew's newest, then the most liked.
inline constexpr std::array<std::string_view, 0> featured_packs{};
inline const CrewMember* crew_member(std::string_view team) {
    for (const auto& member : crew_members)
        if (member.team.size() == team.size() && std::equal(team.begin(), team.end(), member.team.begin(), [](char a, char b) {
                return (a >= 'A' && a <= 'Z' ? a + 32 : a) == (b >= 'A' && b <= 'Z' ? b + 32 : b);
            }))
            return &member;
    return nullptr;
}

struct Pack {
    std::string full_name;      // Owner-Name
    std::string owner, name;    // the team, and the package's name
    std::string title;          // the name, with spaces
    std::string version;        // its newest
    std::string description;    // its short description, without the studio's tag
    std::string icon_url, download_url, package_url;
    std::string updated;        // when its newest version came out ("2026-10-23")
    std::string created;        // when it first came out ("2026-10-23"): TOP OF THE WEEK and OF THE MONTH go by it
    std::uint64_t downloads{};  // every version's together
    int rating{};
    std::string icon_file;      // its icon once downloaded (UTF-8 path), "" until then
    bool favorite{}, mine{};    // favorite: liked here (the heart)
    bool crew{};                // made by one of THE CREW
    std::string installed;      // the version installed here, "" none
    std::string installed_as;   // the pack it became in the library ("" not known: installed before this was kept)
    // Its README, read in the background for every pack (so a search finds a pack by the tricks in it): what its maker
    // says of it and its tricks, a line each. `readme` 0 not read yet, 1 being read, 2 read, 3 could not be read.
    std::string about;
    std::vector<std::string> tricks;
    int readme{};
    // One of the samples (show_samples): `sample` is its place in the list + 1, `sample_days` how old it is.
    int sample{}, sample_days{};
    std::string stance; // what its tag says (ALL STANCES, GOOFY...), "" not known
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
    bool like_prompt{true}; // a like asks to like it on Thunderstore too
    Job job;
    Detail detail;
};
View view();
// The listing, fetched in the background: on opening the MARKET (when it is older than a few minutes) or when asked.
void refresh(bool force);
// Each pack's icon downloaded in the background, for the cards.
void want_icons(const std::vector<std::string>& full_names);
void load_detail(const std::string& full_name);
// The packs' READMEs read in the background (kept on disk by version): a pack's `about` and `tricks` once read.
void want_readmes(const std::vector<std::string>& full_names);
// Where a sample's cover is kept (made by the overlay's cover designer the first time the shop is opened).
std::filesystem::path sample_cover(std::size_t index);
void set_favorite(const std::string& full_name, bool on); // the heart: liked here
// Whether a like asks to like the pack on Thunderstore too. (Thunderstore takes likes only from a person signed in on
// its site, never from an app's token, so the studio opens the pack's page there for it.)
void set_like_prompt(bool on);
// The pack's page on Thunderstore, in the browser: thunderstore.io only.
void open_page(const std::string& full_name);
// A team's page on Thunderstore (its packs in the ReSkate community): THE CREW's link on ABOUT.
inline std::string team_page(std::string_view team) { return std::string(site) + "/c/" + community + "/p/" + std::string(team) + "/"; }
void open_team_page(std::string_view team); // in the browser; a team's name of letters, figures and _ only
// Downloads the pack and puts it in the library (the studio's import), in the background.
void install(const std::string& full_name);
// Links a Thunderstore team for publishing: its name and a service account token of it.
bool link_account(const std::string& team, const std::string& token, std::string& message);
void unlink_account();
// Builds the pack as a Thunderstore package and publishes it, in the background. `pack` is a pack of the library;
// `version` the version it goes up as ("" the next one). A pack published before goes to the same package (an update);
// `package` (Team-Name) names the package to update when the pack does not know it (one uploaded by hand).
void publish(const std::string& pack, const std::string& version = {}, const std::string& package = {});
// The same package written to the exports folder, to upload on thunderstore.io by hand. Returns its path, "" when not.
std::string export_package(const std::string& pack, std::string& message, const std::string& version = {});
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

// Thunderstore's names and versions as they may go into an address: a team's or a package's name is letters, digits
// and _, a version digits and dots.
inline bool plain_name(std::string_view s) {
    if (s.empty() || s.size() > 128) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
inline bool plain_version(std::string_view s) {
    if (s.empty() || s.size() > 32 || s.front() == '.' || s.back() == '.') return false;
    for (const char c : s)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return true;
}
// The version a package's icon is of: Thunderstore names it <Team>-<Name>-<version>.png. "" when it is not that.
inline std::string version_from_icon(std::string_view icon_url, std::string_view full_name) {
    const auto slash = icon_url.find_last_of('/');
    if (slash == std::string_view::npos) return {};
    const auto file = icon_url.substr(slash + 1);
    const std::string head = std::string(full_name) + "-";
    if (full_name.empty() || !file.starts_with(head) || !file.ends_with(".png")) return {};
    const auto version = file.substr(head.size(), file.size() - head.size() - 4);
    return plain_version(version) ? std::string(version) : std::string{};
}

// Whether version `a` comes after `b` (1.0.10 after 1.0.9). Versions not in that form come after nothing.
inline bool version_after(const std::string& a, const std::string& b) {
    int a1 = 0, a2 = 0, a3 = 0, b1 = 0, b2 = 0, b3 = 0;
    if (!plain_version(a) || sscanf_s(a.c_str(), "%d.%d.%d", &a1, &a2, &a3) != 3) return false;
    if (b.empty()) return true;
    if (sscanf_s(b.c_str(), "%d.%d.%d", &b1, &b2, &b3) != 3) return true;
    return a1 != b1 ? a1 > b1 : a2 != b2 ? a2 > b2 : a3 > b3;
}
// Days from 1970-01-01 to a date written 2026-10-23 (only its first ten letters are read); -1 when it is not one.
inline long long day_number(std::string_view date) {
    int y = 0, m = 0, d = 0;
    if (date.size() < 10 || date[4] != '-' || date[7] != '-') return -1;
    for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
        if (date[i] < '0' || date[i] > '9') return -1;
    y = (date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 + (date[3] - '0');
    m = (date[5] - '0') * 10 + (date[6] - '0');
    d = (date[8] - '0') * 10 + (date[9] - '0');
    if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
    // Howard Hinnant's days_from_civil.
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (static_cast<unsigned>(m) + (m > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

// The next version: one past the one published last, else the pack's own.
inline std::string next_version(const std::string& version, const std::string& published) {
    int major = 1, minor = 0, patch = 0;
    if (sscanf_s(version.c_str(), "%d.%d.%d", &major, &minor, &patch) != 3) major = 1, minor = 0, patch = 0;
    if (!published.empty() && published == std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch)) ++patch;
    return std::to_string(std::max(0, major)) + "." + std::to_string(std::max(0, minor)) + "." + std::to_string(std::max(0, patch));
}
} // namespace dingosdk::anim_studio::market
