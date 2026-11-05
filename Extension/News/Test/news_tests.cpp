// What the front page in the pause menu's HUB tab reads: a news post's optional date, and ReSkate's
// releases on GitHub as patch notes (their version, date and highlights).
#include "Extension/News/patch_notes.h"
#include "Extension/Profile/local_profile.h"

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
profile::NewsFeed feed_with(const std::string& post) {
    return profile::parse_news(Json::parse(R"({"enabled": true, "posts": [)" + post + "]}"));
}
} // namespace

int main() {
    // ---- the news date
    check(profile::iso_date("2026-10-24") == "2026-10-24", "a plain date is read");
    check(profile::iso_date("2026-10-24T18:30:00Z") == "2026-10-24", "a date and time gives its date");
    check(profile::iso_date("2028-02-29") == "2028-02-29" && profile::iso_date("2026-02-29").empty(), "the 29th of February only in a leap year");
    check(profile::iso_date("2026-13-01").empty() && profile::iso_date("2026-04-31").empty(), "a month or day that does not exist is no date");
    check(profile::iso_date("24/10/2026").empty() && profile::iso_date("2026-10-2").empty() && profile::iso_date("").empty(),
          "anything that is not an ISO date is no date");
    check(profile::iso_date("2026-10-24x").empty(), "a date with something else after it is no date");
    {
        const auto feed = feed_with(R"({"id": "a", "title": "A", "date": "2026-10-24"}, {"id": "b", "title": "B"})");
        check(feed.posts.size() == 2 && feed.posts[0].date == "2026-10-24" && feed.posts[1].date.empty(), "a post's date is optional");
    }
    {
        const auto feed = feed_with(R"({"id": "a", "title": "A", "date": "last Tuesday"})");
        check(feed.posts.size() == 1 && feed.posts[0].date.empty(), "a date that cannot be read is left off, the post kept");
    }
    bool refused{};
    try {
        feed_with(R"({"id": "a", "title": "A", "date": 20261024})");
    } catch (const std::exception&) {
        refused = true;
    }
    check(refused, "a date that is not text is refused like any other field");
    check(news::date_label("2026-10-24") == "OCT 24, 2026" && news::date_label("2026-01-05T00:00:00Z") == "JAN 5, 2026", "a date as the menus show it");
    check(news::date_label("soon").empty(), "no label for what is not a date");

    // ---- the releases
    const auto releases = news::parse_releases(R"([
        {"tag_name": "v2.1.4-2", "name": "ReSkate 2.1.4-2", "draft": false, "published_at": "2026-10-30T12:00:00Z",
         "body": "## What's new\r\n- Other players' **fast travel** flags on your map\r\n- Ignore a player: nothing of theirs shows\r\n\r\n## Fixes\r\n* Plugins no longer stop on a slow disk\r\nThanks to everyone who tested."},
        {"tag_name": "v2.2.0", "name": "", "draft": true, "published_at": null, "body": "- not out yet"},
        {"tag_name": "v2.1.4", "name": "", "draft": false, "published_at": null, "created_at": "2026-10-20T09:00:00Z", "body": ""},
        {"name": "no tag", "body": "- skipped"},
        "not a release"
    ])");
    check(releases.size() == 2, "drafts, releases without a tag and anything else are left out");
    if (releases.size() == 2) {
        const auto& latest = releases[0];
        check(latest.tag == "v2.1.4-2" && latest.name == "ReSkate 2.1.4-2" && latest.date == "2026-10-30", "a release's tag, name and date");
        check(news::release_version(latest) == "2.1.4-2", "the version is the tag without its v");
        const auto highlights = news::release_highlights(latest, 10);
        check(highlights.size() == 3 && highlights[0] == "Other players' fast travel flags on your map" &&
                  highlights[2] == "Plugins no longer stop on a slow disk",
              "the highlights are the bullet lines, in order, without markdown's marks");
        check(news::release_highlights(latest, 2).size() == 2, "at most as many highlights as asked for");
        check(!latest.lines.empty() && latest.lines.front().kind == thunderstore::ReadmeLine::Kind::heading && latest.lines.back().text == "Thanks to everyone who tested.",
              "the notes keep their headings and text");
        check(releases[1].name == "v2.1.4" && releases[1].date == "2026-10-20" && releases[1].lines.empty(),
              "a release with no name is called by its tag, and dated when it was made if not published");
    }
    check(news::parse_releases(R"([{"tag_name": "a"}, {"tag_name": "b"}, {"tag_name": "c"}])", 2).size() == 2, "at most as many releases as asked for");
    check(news::release_version(news::Release{"version-3", "", "", {}}) == "version-3", "a tag that is not v and a number stays as it is");
    refused = false;
    try {
        news::parse_releases(R"({"message": "API rate limit exceeded"})");
    } catch (const std::exception&) {
        refused = true;
    }
    check(refused, "GitHub's answer when it refuses is not taken for an empty list");

    if (failures) return 1;
    std::cout << "news tests passed\n";
    return 0;
}
