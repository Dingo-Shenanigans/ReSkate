// Thunderstore listing: parsing, gzip chunks, versions and the folder a package installs into.
#include "Launcher/thunderstore.h"

#include <miniz.h>

#include <iostream>
#include <string>

using namespace dingosdk;

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

template<class F> std::string error_of(F&& run) {
    try { run(); } catch (const std::exception& failure) { return failure.what(); }
    return {};
}

// A gzip member holding `text` in one stored (uncompressed) deflate block.
std::string gzip(std::string_view text, bool with_name = false) {
    std::string out{'\x1f', '\x8b', '\x08', static_cast<char>(with_name ? 0x08 : 0), 0, 0, 0, 0, 0, '\xff'};
    if (with_name) out += std::string("listing.json") + '\0';
    const auto length = static_cast<unsigned>(text.size());
    out += '\x01';
    out += static_cast<char>(length & 0xff);
    out += static_cast<char>(length >> 8 & 0xff);
    out += static_cast<char>(~length & 0xff);
    out += static_cast<char>(~length >> 8 & 0xff);
    out += text;
    const auto crc = mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    for (int i = 0; i < 4; ++i) out += static_cast<char>(crc >> (8 * i) & 0xff);
    for (int i = 0; i < 4; ++i) out += static_cast<char>(length >> (8 * i) & 0xff);
    return out;
}

constexpr char listing[] = R"([
  {"name": "Desert_Springs", "full_name": "Zee-Desert_Springs", "owner": "Zee",
   "package_url": "https://thunderstore.io/c/reskate/p/Zee/Desert_Springs/",
   "date_created": "2026-10-01T10:00:00Z", "date_updated": "2026-10-02T09:00:00Z",
   "rating_score": 4, "is_pinned": false, "is_deprecated": false, "has_nsfw_content": false,
   "categories": ["Mods", "Maps"],
   "versions": [
     {"name": "Desert_Springs", "full_name": "Zee-Desert_Springs-1.9.0", "description": "Old",
      "icon": "https://gcdn.thunderstore.io/live/repository/icons/Zee-Desert_Springs-1.9.0.png",
      "version_number": "1.9.0", "dependencies": [], "downloads": 10,
      "download_url": "https://thunderstore.io/package/download/Zee/Desert_Springs/1.9.0/",
      "date_created": "2026-10-01T10:00:00Z", "website_url": "", "is_active": true, "file_size": 700000000},
     {"name": "Desert_Springs", "full_name": "Zee-Desert_Springs-1.10.0", "description": "A desert park.",
      "icon": "https://gcdn.thunderstore.io/live/repository/icons/Zee-Desert_Springs-1.10.0.png",
      "version_number": "1.10.0", "dependencies": [], "downloads": 5,
      "download_url": "https://thunderstore.io/package/download/Zee/Desert_Springs/1.10.0/",
      "date_created": "2026-10-02T09:00:00Z", "website_url": "https://example.com", "is_active": true,
      "file_size": 5368709120},
     {"name": "Desert_Springs", "full_name": "Zee-Desert_Springs-2.0.0", "description": "Pulled",
      "version_number": "2.0.0", "download_url": "https://thunderstore.io/package/download/Zee/Desert_Springs/2.0.0/",
      "is_active": false}
   ]},
  {"name": "Bad Name", "full_name": "Zee-Bad Name", "owner": "Zee", "versions": []},
  {"name": "No_Versions", "full_name": "Zee-No_Versions", "owner": "Zee", "versions": []},
  {"name": "Plain", "full_name": "Other-Plain", "owner": "Other",
   "versions": [{"version_number": "1.0.0", "download_url": "http://insecure.example/plain.zip"}]},
  "not a package",
  {"name": "Low_Cam", "full_name": "SunJay-Low_Cam", "owner": "SunJay", "is_deprecated": true,
   "versions": [{"version_number": "0.1.0", "description": "Low camera",
                 "download_url": "https://thunderstore.io/package/download/SunJay/Low_Cam/0.1.0/",
                 "icon": "http://not-https/icon.png", "downloads": 3}]}
])";

} // namespace

int main() {
    // ------------------------------------------------ listing
    const auto packages = thunderstore::parse_listing(listing);
    check(packages.size() == 2, "Malformed rows, packages without versions and insecure downloads are skipped");
    const auto& desert = packages.at(0);
    check(desert.full_name == "Zee-Desert_Springs" && desert.owner == "Zee" && desert.title() == "Desert Springs",
          "Names are read and titles show underscores as spaces");
    check(desert.versions.size() == 2 && desert.latest().number == "1.10.0",
          "Inactive versions are dropped and the newest version comes first");
    check(desert.latest().file_size == 5368709120ull, "Sizes over 4 GB survive");
    check(desert.downloads == 15 && desert.in_category("Maps") && !desert.in_category("Audio"),
          "Downloads add up across versions and categories are listed");
    const auto& cam = packages.at(1);
    check(cam.deprecated && cam.latest().icon.empty(), "Flags are read and non-HTTPS icons are dropped");
    check(!error_of([] { thunderstore::parse_listing(R"({"error": "No cache available"})"); }).empty(),
          "A listing that is not an array is an error");
    check(thunderstore::parse_listing("[]").empty(), "An empty community lists nothing");

    // ------------------------------------------------ index and gzip
    const auto chunk = gzip(listing, true);
    check(thunderstore::gunzip(chunk, 1 << 20) == listing, "A gzip chunk (with a file name) unpacks");
    check(thunderstore::gunzip("[]", 16) == "[]", "Plain JSON passes through");
    check(!error_of([&] { thunderstore::gunzip(chunk, 100); }).empty(), "Output past the limit is refused");
    auto damaged = chunk;
    damaged[damaged.size() - 9] ^= 0x20;
    check(!error_of([&] { thunderstore::gunzip(damaged, 1 << 20); }).empty(), "A damaged chunk fails its CRC check");
    check(!error_of([&] { thunderstore::gunzip(chunk.substr(0, 12), 1 << 20); }).empty(), "A truncated chunk is refused");
    const auto index = thunderstore::parse_index(thunderstore::gunzip(gzip(
        R"(["https://ccdn.thunderstore.io/live/blob-storage/sha256/abc.blob"])"), 1024));
    check(index.size() == 1 && index[0] == L"https://ccdn.thunderstore.io/live/blob-storage/sha256/abc.blob",
          "The index lists chunk URLs");
    check(!error_of([] { thunderstore::parse_index(R"(["http://insecure/blob"])"); }).empty(),
          "Non-HTTPS chunks are refused");

    // ------------------------------------------------ versions
    check(thunderstore::compare_versions("1.10.0", "1.9.0") > 0, "Version parts compare as numbers");
    check(thunderstore::compare_versions("1.0.0", "1.0") == 0, "Missing parts count as zero");
    check(thunderstore::compare_versions("01.2.3", "1.2.3") == 0, "Leading zeros do not matter");
    check(thunderstore::compare_versions("2.0.0", "10.0.0") < 0, "A shorter number is smaller");
    check(thunderstore::compare_versions("", "0.0.1") < 0, "No version is older than any version");

    // ------------------------------------------------ installed copies
    check(thunderstore::folder_for("Zee-Desert_Springs") == "Zee-Desert_Springs", "Packages install as Namespace-Name");
    check(thunderstore::folder_for(std::string(100, 'a')).size() == 64, "Folder names keep to the Mods name limit");
    thunderstore::Installed installed{{"Zee-Desert_Springs", "1.9.0"}, {"SunJay-Low_Cam", "0.1.0"}};
    check(thunderstore::update_available(desert, installed), "An older installed copy has an update");
    check(!thunderstore::update_available(cam, installed), "The same version has none");
    installed["Zee-Desert_Springs"] = "";
    check(!thunderstore::update_available(desert, installed), "An installed copy without a version is left alone");
    installed.erase("Zee-Desert_Springs");
    check(!thunderstore::update_available(desert, installed), "A package that is not installed has no update");

    // ------------------------------------------------ README
    check(thunderstore::readme_url(desert) == L"https://thunderstore.io/api/experimental/package/" +
              std::wstring(desert.owner.begin(), desert.owner.end()) + L"/" + std::wstring(desert.name.begin(), desert.name.end()) + L"/" +
              std::wstring(desert.latest().number.begin(), desert.latest().number.end()) + L"/readme/",
          "The README URL names the owner, the package and its newest version");
    check(thunderstore::parse_readme(R"({"markdown": "# Hi\nthere"})") == "# Hi\nthere", "The README's markdown is read");
    check(thunderstore::parse_readme(R"({"markdown": null})").empty() && thunderstore::parse_readme("{}").empty(),
          "A package without a README has an empty one");
    check(!error_of([] { (void)thunderstore::parse_readme("[]"); }).empty(), "A README answer that is not an object is refused");
    {
        using Kind = thunderstore::ReadmeLine::Kind;
        const auto readme = thunderstore::readme_lines(
            "\n\n# Desert **Springs** #\r\n"
            "![banner](https://example.com/a.png)\n"
            "A [big map](https://example.com) with `ramps`, *bowls* and a snake_run.\n\n\n"
            "- first\n* second <b>bold</b>\n"
            "```\nmp host   --now\n```\n"
            "---\n"
            "> quoted &amp; kept\n"
            "| Key | Does |\n|-----|------|\n| F1 | Yes |\n"
            "<img src=\"x.png\">\n"
            "1. numbered stays as written\n");
        const std::vector<thunderstore::ReadmeLine> expected{
            {Kind::heading, "Desert Springs"},
            {Kind::text, "A big map with ramps, bowls and a snake_run."},
            {Kind::gap, ""},
            {Kind::bullet, "first"},
            {Kind::bullet, "second bold"},
            {Kind::code, "mp host   --now"},
            {Kind::rule, ""},
            {Kind::text, "quoted & kept"},
            {Kind::text, "Key   Does"},
            {Kind::text, "F1   Yes"},
            {Kind::text, "1. numbered stays as written"},
        };
        check(readme.lines == expected && !readme.cut, "A README's markdown is laid out as plain lines");
        std::string many;
        for (int i = 0; i < 50; ++i) many += "line " + std::to_string(i) + "\n";
        const auto cut = thunderstore::readme_lines(many, 10);
        check(cut.lines.size() == 10 && cut.cut && cut.lines.back().text == "line 9", "A long README is cut at the limit and says so");
        check(thunderstore::readme_lines("").lines.empty() && thunderstore::readme_lines("\n \n![x](y)\n").lines.empty(),
              "A README with nothing to show has no lines");
        const auto wide = thunderstore::readme_lines(std::string(1999, 'a') + "\xC3\xA9\xC3\xA9");
        check(wide.lines.size() == 1 && wide.lines[0].text == std::string(1999, 'a'), "An overlong line is cut between characters");
    }

    // ------------------------------------------------ community
    check(thunderstore::listing_index_url("reskate") == L"https://thunderstore.io/c/reskate/api/v1/package-listing-index/",
          "The listing index URL");
    check(thunderstore::community_page("reskate") == L"https://thunderstore.io/c/reskate/", "The community page URL");

    if (failures) {
        std::cerr << failures << " Thunderstore check(s) failed\n";
        return 1;
    }
    std::cout << "Thunderstore checks passed.\n";
    return 0;
}
