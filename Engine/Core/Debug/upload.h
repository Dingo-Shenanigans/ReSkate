#pragma once
#include "Engine/Core/Json/json.h"
#include <Windows.h>
#include <filesystem>
#include <functional>
#include <string>

namespace dingosdk::crash_report {
inline constexpr std::uintmax_t max_dump_bytes = 128ull * 1024 * 1024;
inline constexpr std::size_t max_pending_reports = 20;
struct Endpoint { std::wstring host, resource; USHORT port{}; };
struct UploadResult { bool accepted{}; DWORD http_status{}, error{}; std::string report_id; };
bool parse_endpoint(const std::wstring& url, Endpoint& endpoint);
bool accepted_response(DWORD status, std::string_view response, std::string& report_id);
std::filesystem::path queue_directory(const std::filesystem::path& root, const std::wstring& url);
UploadResult upload_report(const std::wstring& url, const std::filesystem::path& dump, const Json& attributes);
using Sender = std::function<UploadResult(const std::wstring&, const std::filesystem::path&, const Json&)>;
// Cross-process queue lock prevents the launcher and game helpers submitting the same dump.
std::size_t upload_pending(const std::wstring& url, const std::filesystem::path& directory,
    const Sender& sender = upload_report) noexcept;
}
