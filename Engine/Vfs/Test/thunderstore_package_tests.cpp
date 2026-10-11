// One Thunderstore package looked up by name (Engine/Vfs/thunderstore_package.h): the name a
// host gives, and what Thunderstore's answer has to say before the game downloads it.
#include "Engine/Vfs/thunderstore_package.h"

#include <iostream>
#include <string>

using namespace dingosdk;
namespace package = thunderstore_package;

namespace {
int failures = 0;
void check(bool condition, const char* what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

// Thunderstore's answer for a package, as /api/experimental/package/<owner>/<name>/ gives it.
std::string answer(std::string_view owner, std::string_view name, std::string_view version, std::string_view community,
                   std::string_view category, std::string_view review, bool deprecated = false) {
    return std::string("{\"namespace\":\"") + std::string(owner) + "\",\"name\":\"" + std::string(name) +
           "\",\"full_name\":\"x\",\"is_deprecated\":" + (deprecated ? "true" : "false") +
           ",\"latest\":{\"version_number\":\"" + std::string(version) +
           "\",\"icon\":\"https://gcdn.thunderstore.io/live/repository/icons/x.png\",\"description\":\"A plaza\\nby the sea\",\"download_url\":\"https://elsewhere.example/steal.zip\",\"is_active\":true},\"community_listings\":[{\"has_nsfw_content\":false,"
           "\"categories\":[\"" + std::string(category) + "\"],\"community\":\"" + std::string(community) +
           "\",\"review_status\":\"" + std::string(review) + "\"}]}";
}
}

int main() {
    {
        const auto plain = package::parse_name("Sandos-Vancouver_Plaza");
        check(plain && plain->owner == "Sandos" && plain->name == "Vancouver_Plaza" && plain->version.empty() &&
              plain->folder() == "Sandos-Vancouver_Plaza", "a package name read");
        const auto versioned = package::parse_name("Sandos-Vancouver_Plaza-2.0.0");
        check(versioned && versioned->version == "2.0.0" && versioned->folder() == "Sandos-Vancouver_Plaza", "and with its version");
        for (const char* bad : {"", "Sandos", "Sandos-", "-Plaza", "San dos-Plaza", "Sandos-Plaza-2.0", "Sandos-Plaza-2.0.0.1", "Sandos-Plaza-two",
                                "../x-y", "Sandos-Plaza/..", "Sandos-Pla.za", "a-b-1.2.3-4"})
            check(!package::parse_name(bad), bad);
        const auto name = *versioned;
        check(package::details_url(name) == L"https://thunderstore.io/api/experimental/package/Sandos/Vancouver_Plaza/", "where it is asked about");
        check(package::download_url(name, "2.0.0") == L"https://thunderstore.io/package/download/Sandos/Vancouver_Plaza/2.0.0/",
              "where it is downloaded from");
    }
    {
        const package::Name newest{"Sandos", "Vancouver_Plaza", ""}, pinned{"Sandos", "Vancouver_Plaza", "1.4.0"};
        const auto listed = answer("Sandos", "Vancouver_Plaza", "2.0.0", "reskate", "Maps", "unreviewed");
        auto choice = package::choose(listed, newest, "reskate");
        check(choice.ok && choice.version == "2.0.0" && choice.newest == "2.0.0", "a listed map is taken, its newest version");
        check(choice.description == "A plaza by the sea", "with what it says of itself, on one line");
        check(choice.icon == "https://gcdn.thunderstore.io/live/repository/icons/x.png", "and its icon, from Thunderstore's own host");
        {
            auto elsewhere = listed;
            const std::string host = "gcdn.thunderstore.io";
            elsewhere.replace(elsewhere.find(host), host.size(), "thunderstore.io.example.com");
            const auto other = package::choose(elsewhere, newest, "reskate");
            check(other.ok && other.icon.empty(), "an icon anywhere else is not fetched");
        }
        choice = package::choose(listed, pinned, "reskate");
        check(choice.ok && choice.version == "1.4.0" && choice.newest == "2.0.0", "or the version the host has, with the newest to fall back on");
        check(package::choose(answer("sandos", "vancouver_plaza", "2.0.0", "ReSkate", "maps", "approved"), newest, "reskate").ok,
              "names compare whatever their case");
        const auto refused = [&](const std::string& text, const char* what) {
            const auto result = package::choose(text, newest, "reskate");
            check(!result.ok && !result.reason.empty() && result.version.empty(), what);
        };
        refused(answer("Someone", "Else", "2.0.0", "reskate", "Maps", "unreviewed"), "another package's answer is refused");
        refused(answer("Sandos", "Vancouver_Plaza", "2.0.0", "reskate", "Cosmetics", "unreviewed"), "a package that is not a map is refused");
        refused(answer("Sandos", "Vancouver_Plaza", "2.0.0", "othergame", "Maps", "unreviewed"), "a package of another community is refused");
        refused(answer("Sandos", "Vancouver_Plaza", "2.0.0", "reskate", "Maps", "rejected"), "a package the moderators rejected is refused");
        refused(answer("Sandos", "Vancouver_Plaza", "2.0.0", "reskate", "Maps", "unreviewed", true), "a deprecated package is refused");
        refused(answer("Sandos", "Vancouver_Plaza", "latest", "reskate", "Maps", "unreviewed"), "a version that is not one is refused");
        {
            auto marked = listed;
            const std::string off = "\"has_nsfw_content\":false";
            marked.replace(marked.find(off), off.size(), "\"has_nsfw_content\":true");
            refused(marked, "a package marked NSFW is refused");
        }
        refused("{\"namespace\":\"Sandos\",\"name\":\"Vancouver_Plaza\"}", "an answer with no listing is refused");
        refused("not json", "an answer that is not JSON is refused");
        refused("[]", "an answer that is not an object is refused");
    }
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "thunderstore package tests passed\n";
    return 0;
}
