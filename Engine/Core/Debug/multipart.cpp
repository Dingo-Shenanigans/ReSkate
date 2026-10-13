#include "multipart.h"
#include "upload.h"
#include <algorithm>
#include <format>
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
MultipartReport::MultipartReport(const std::filesystem::path& dump, const Json& attributes, const std::string& boundary) {
    if (!attributes.is_object() || boundary.empty() || boundary.size() > 70 ||
            boundary.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::string::npos) {
        error_ = ERROR_INVALID_PARAMETER; return;
    }
    dump_.value = CreateFileW(dump.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!file_length(dump_.value, max_dump_bytes, dump_bytes_) || !dump_bytes_) { error_ = ERROR_FILE_INVALID; return; }
    for (const auto& [key, value] : attributes.items()) {
        if (key.empty() || key.size() > 128 || key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos ||
                !value.is_string() || value.string().size() > 4096 || value.string().find(boundary) != std::string::npos) {
            error_ = ERROR_INVALID_DATA; return;
        }
    }
    // Sentry reads event attributes from the "sentry" field; attributes become searchable tags.
    Json event = Json::object();
    event["tags"] = attributes;
    if (const auto version = attributes.value("version", ""); !version.empty()) event["release"] = "reskate@" + version;
    const auto fields = event.dump();
    if (fields.size() > 65536 || fields.find(boundary) != std::string::npos) { error_ = ERROR_INVALID_DATA; return; }
    prefix_ = std::format("--{}\r\nContent-Disposition: form-data; name=\"sentry\"\r\n\r\n{}\r\n", boundary, fields);
    prefix_ += std::format("--{}\r\nContent-Disposition: form-data; name=\"upload_file_minidump\"; filename=\"crash.dmp\"\r\nContent-Type: application/octet-stream\r\n\r\n", boundary);
    const auto metadata = attributes.dump();
    if (metadata.size() > 65536) { error_ = ERROR_INVALID_DATA; return; }
    attachments_ = std::format("\r\n--{}\r\nContent-Disposition: form-data; name=\"attachment_report.json\"; filename=\"report.json\"\r\nContent-Type: application/json\r\n\r\n{}", boundary, metadata);
    const auto log_status = attributes.value("attachment.log", "unavailable");
    if (log_status == "complete" || log_status == "tail") {
        log_.value = CreateFileW(log_attachment_path(dump).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        // If a queued attachment becomes unreadable, retain the report for retry
        // instead of confirming an upload that silently omitted its log.
        if (!file_length(log_.value, max_log_bytes, log_bytes_)) { error_ = ERROR_FILE_INVALID; return; }
        attachments_ += std::format("\r\n--{}\r\nContent-Disposition: form-data; name=\"attachment_ReSkate.log\"; filename=\"ReSkate.log\"\r\nContent-Type: text/plain\r\n\r\n", boundary);
    }
    suffix_ = std::format("\r\n--{}--\r\n", boundary);
    const auto total = prefix_.size() + static_cast<std::uint64_t>(dump_bytes_) + attachments_.size() + log_bytes_ + suffix_.size();
    if (total > std::numeric_limits<DWORD>::max()) { error_ = ERROR_FILE_TOO_LARGE; return; }
    size_ = static_cast<DWORD>(total);
}
bool MultipartReport::write(const std::function<bool(const void*, DWORD)>& sink) {
    return !error_ && sink(prefix_.data(), static_cast<DWORD>(prefix_.size())) && stream_file(dump_.value, dump_bytes_, sink) &&
        sink(attachments_.data(), static_cast<DWORD>(attachments_.size())) && stream_file(log_.value, log_bytes_, sink) &&
        sink(suffix_.data(), static_cast<DWORD>(suffix_.size()));
}
}
