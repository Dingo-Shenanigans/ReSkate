#pragma once
#include "protocol.h"
#include "Engine/Core/Json/json.h"
#include <filesystem>
#include <functional>

namespace dingosdk::crash_report {
inline constexpr DWORD max_log_bytes = 8 * 1024 * 1024;
enum class LogSnapshot { unavailable, complete, tail };
std::filesystem::path log_attachment_path(const std::filesystem::path& dump);
// Snapshot before publishing the dump; retrying must never read a later session's log.
LogSnapshot snapshot_log(const std::filesystem::path& source, const std::filesystem::path& dump) noexcept;

// A queued report as one Sentry envelope, as sentry-native sends a crash: the event (tags, release,
// time and the details report_details.h made), the minidump, report.json and the log. Sentry's
// minidump endpoint streams attachments other than the dump to an object store, which a
// self-hosted Sentry without one drops ("upload_failed"); an envelope's are kept.
// Keeps the queued files open and unchanged until the complete request is sent.
class ReportEnvelope {
public:
    // event_id: 32 lowercase hexadecimal digits.
    ReportEnvelope(const std::filesystem::path& dump, const Json& attributes, const std::string& event_id);
    DWORD error() const { return error_; }
    DWORD size() const { return size_; }
    bool write(const std::function<bool(const void*, DWORD)>& sink);
private:
    Handle dump_, log_;
    DWORD dump_bytes_{}, log_bytes_{}, size_{}, error_{};
    std::string prefix_, middle_, suffix_;
};
// The envelope endpoint of a Sentry minidump endpoint URL (".../api/<project>/minidump/?sentry_key=..."
// -> ".../api/<project>/envelope/?sentry_key=..."), or empty when it is not one.
std::wstring envelope_endpoint(std::wstring_view minidump_url);
}
