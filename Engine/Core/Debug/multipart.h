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

// Keeps the queued files open and unchanged until the complete request is sent.
class MultipartReport {
public:
    MultipartReport(const std::filesystem::path& dump, const Json& attributes, const std::string& boundary);
    DWORD error() const { return error_; }
    DWORD size() const { return size_; }
    bool write(const std::function<bool(const void*, DWORD)>& sink);
private:
    Handle dump_, log_;
    DWORD dump_bytes_{}, log_bytes_{}, size_{}, error_{};
    std::string prefix_, attachments_, suffix_;
};
}
