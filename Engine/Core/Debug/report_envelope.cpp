#include "report_envelope.h"
#include "upload.h"
#include "report_details.h"
#include <algorithm>
#include <format>
#include <fstream>
#include <limits>

namespace dingosdk::crash_report {
namespace {
bool stream_file(HANDLE file, DWORD bytes, const std::function<bool(const void*, DWORD)>& sink) {
    std::array<char, 65536> buffer{};
    while (bytes) {
        DWORD read{};
        const auto wanted = std::min<DWORD>(bytes, static_cast<DWORD>(buffer.size()));
        if (!ReadFile(file, buffer.data(), wanted, &read, nullptr) || read != wanted || !sink(buffer.data(), read)) return false;
        bytes -= read;
    }
    return true;
}
bool file_length(HANDLE file, std::uintmax_t maximum, DWORD& bytes) {
    LARGE_INTEGER length{};
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &length) || length.QuadPart < 0 ||
            static_cast<std::uintmax_t>(length.QuadPart) > maximum) return false;
    bytes = static_cast<DWORD>(length.QuadPart);
    return true;
}
// When the dump was written, as seconds since 1970: the crash's time, however late it is sent.
double written_at(HANDLE file) {
    FILETIME created{}, accessed{}, written{};
    if (!GetFileTime(file, &created, &accessed, &written)) return 0.0;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = written.dwLowDateTime;
    ticks.HighPart = written.dwHighDateTime;
    return static_cast<double>(ticks.QuadPart) / 1e7 - 11644473600.0;
}
// An envelope item's header line: its type, length and what else Sentry reads of it.
std::string item_header(Json header, std::uint64_t length) {
    header["length"] = length;
    return header.dump() + "\n";
}
// The rest of the event the report made (report_details.h): the log's last lines, the mods, who and
// how long. A report queued without it, or with one that is unreadable, goes without.
void merge_details(const std::filesystem::path& dump, Json& event) {
    const auto path = event_details_path(dump);
    std::error_code missing;
    const auto size = std::filesystem::file_size(path, missing);
    if (missing || size > max_event_details_bytes) return;
    std::ifstream input(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!input.read(text.data(), static_cast<std::streamsize>(text.size()))) return;
    try {
        const auto details = Json::parse(text, {max_event_details_bytes, 16, 65536});
        if (!details.is_object()) return;
        // (level and fingerprint: a stuck load's report is a warning of its own kind, not a crash.)
        for (const auto& [key, value] : details.items())
            if (key == "breadcrumbs" || key == "contexts" || key == "user" || key == "extra" ||
                (key == "level" && value.is_string()) || (key == "fingerprint" && value.is_array()))
                event[key] = value;
    } catch (...) {}
}
}
std::filesystem::path log_attachment_path(const std::filesystem::path& dump) { return dump.wstring() + L".log"; }
LogSnapshot snapshot_log(const std::filesystem::path& source, const std::filesystem::path& dump) noexcept {
    try {
        Handle input(CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        LARGE_INTEGER length{};
        if (input.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(input.value, &length) || length.QuadPart < 0)
            return LogSnapshot::unavailable;
        const bool truncated = length.QuadPart > max_log_bytes;
        const auto bytes = static_cast<DWORD>(std::min<LONGLONG>(length.QuadPart, max_log_bytes));
        LARGE_INTEGER start{}; start.QuadPart = length.QuadPart - bytes;
        if (!SetFilePointerEx(input.value, start, nullptr, FILE_BEGIN)) return LogSnapshot::unavailable;
        const auto destination = log_attachment_path(dump);
        Handle output(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (output.value == INVALID_HANDLE_VALUE) return LogSnapshot::unavailable;
        const auto saved = stream_file(input.value, bytes, [&](const void* data, DWORD count) {
            DWORD written{};
            return WriteFile(output.value, data, count, &written, nullptr) && written == count;
        }) && FlushFileBuffers(output.value);
        CloseHandle(output.release());
        if (!saved) { DeleteFileW(destination.c_str()); return LogSnapshot::unavailable; }
        return truncated ? LogSnapshot::tail : LogSnapshot::complete;
    } catch (...) { return LogSnapshot::unavailable; }
}
ReportEnvelope::ReportEnvelope(const std::filesystem::path& dump, const Json& attributes, const std::string& event_id) {
    if (!attributes.is_object() || event_id.size() != 32 || event_id.find_first_not_of("0123456789abcdef") != std::string::npos) {
        error_ = ERROR_INVALID_PARAMETER; return;
    }
    dump_.value = CreateFileW(dump.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!file_length(dump_.value, max_dump_bytes, dump_bytes_) || !dump_bytes_) { error_ = ERROR_FILE_INVALID; return; }
    for (const auto& [key, value] : attributes.items()) {
        if (key.empty() || key.size() > 128 || key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos ||
                !value.is_string() || value.string().size() > 4096) {
            error_ = ERROR_INVALID_DATA; return;
        }
    }
    // The event: Sentry reads the crash from the minidump attached to it; the attributes are tags.
    Json event = Json::object();
    event["event_id"] = event_id;
    event["platform"] = "native";
    event["level"] = "fatal";
    if (const auto time = written_at(dump_.value); time > 0.0) event["timestamp"] = time;
    event["tags"] = attributes;
    if (const auto version = attributes.value("version", ""); !version.empty()) event["release"] = "reskate@" + version;
    merge_details(dump, event);
    const auto payload = event.dump();
    if (payload.size() > max_event_details_bytes + 65536) { error_ = ERROR_INVALID_DATA; return; }
    Json header = Json::object();
    header["event_id"] = event_id;
    prefix_ = header.dump() + "\n";
    Json event_item = Json::object();
    event_item["type"] = "event";
    event_item["content_type"] = "application/json";
    prefix_ += item_header(std::move(event_item), payload.size()) + payload + "\n";
    Json minidump = Json::object();
    minidump["type"] = "attachment";
    minidump["attachment_type"] = "event.minidump";
    minidump["filename"] = "crash.dmp";
    prefix_ += item_header(std::move(minidump), dump_bytes_);
    const auto metadata = attributes.dump();
    if (metadata.size() > 65536) { error_ = ERROR_INVALID_DATA; return; }
    Json report = Json::object();
    report["type"] = "attachment";
    report["filename"] = "report.json";
    report["content_type"] = "application/json";
    middle_ = "\n" + item_header(std::move(report), metadata.size()) + metadata + "\n";
    const auto log_status = attributes.value("attachment.log", "unavailable");
    if (log_status == "complete" || log_status == "tail") {
        log_.value = CreateFileW(log_attachment_path(dump).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        // If a queued attachment becomes unreadable, retain the report for retry
        // instead of confirming an upload that silently omitted its log.
        if (!file_length(log_.value, max_log_bytes, log_bytes_)) { error_ = ERROR_FILE_INVALID; return; }
        Json log = Json::object();
        log["type"] = "attachment";
        log["filename"] = "ReSkate.log";
        log["content_type"] = "text/plain";
        middle_ += item_header(std::move(log), log_bytes_);
        suffix_ = "\n";
    }
    const auto total = prefix_.size() + static_cast<std::uint64_t>(dump_bytes_) + middle_.size() + log_bytes_ + suffix_.size();
    if (total > std::numeric_limits<DWORD>::max()) { error_ = ERROR_FILE_TOO_LARGE; return; }
    size_ = static_cast<DWORD>(total);
}
bool ReportEnvelope::write(const std::function<bool(const void*, DWORD)>& sink) {
    return !error_ && sink(prefix_.data(), static_cast<DWORD>(prefix_.size())) && stream_file(dump_.value, dump_bytes_, sink) &&
        sink(middle_.data(), static_cast<DWORD>(middle_.size())) && stream_file(log_.value, log_bytes_, sink) &&
        (suffix_.empty() || sink(suffix_.data(), static_cast<DWORD>(suffix_.size())));
}
std::wstring envelope_endpoint(std::wstring_view minidump_url) {
    constexpr std::wstring_view minidump = L"/minidump/";
    const auto at = minidump_url.find(minidump);
    if (at == std::wstring_view::npos) return {};
    return std::wstring(minidump_url.substr(0, at)) + L"/envelope/" + std::wstring(minidump_url.substr(at + minidump.size()));
}
}
