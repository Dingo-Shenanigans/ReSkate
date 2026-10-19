#pragma once
#include "Engine/Core/Json/json.h"
#include <cstddef>
#include <filesystem>
#include <string>

// What a crash report carries to Sentry besides its tags and attachments. Attachments (the log)
// are kept only where the server stores them; these go in the event itself, which it always does.
namespace dingosdk::crash_report {
// <dump>.event.json: the report's event fields other than tags and release (breadcrumbs,
// contexts, user), merged into the event when it is sent.
std::filesystem::path event_details_path(const std::filesystem::path& dump);
inline constexpr std::size_t max_event_details_bytes = 512 * 1024;

// The log's last lines as Sentry breadcrumbs ({"values": [...]}, oldest first): up to `count`
// lines, the debug ones only among the last `recent` (what happened just before is worth all of
// it; further back only what was said above debug). Each line's time, source, level and text.
Json log_breadcrumbs(const std::filesystem::path& log, std::size_t count = 100, std::size_t recent = 25) noexcept;

// Where the dump's exception happened, as "<module file name>+0x<offset>", or empty when the dump
// does not say (no exception, or an address outside every module).
std::string crash_site(const std::filesystem::path& dump) noexcept;

// An ID for this install, random and made once, kept in <directory>/install-id.txt: tells one
// player's crashes from another's without saying who they are. Empty when it cannot be kept.
std::string install_id(const std::filesystem::path& directory) noexcept;

// An accepted report's files go to <its folder>/sent, which keeps the newest `keep` reports: the
// player keeps their crashes after they are sent, whatever the server keeps.
inline constexpr std::size_t kept_sent_reports = 10;
void keep_sent_report(const std::filesystem::path& dump, std::size_t keep = kept_sent_reports) noexcept;
// Every file a queued report has (the dump, its tags, log and event details).
void remove_report_files(const std::filesystem::path& dump) noexcept;
}
