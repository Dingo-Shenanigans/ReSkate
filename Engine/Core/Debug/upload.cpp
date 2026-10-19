#include <Windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include "upload.h"
#include "protocol.h"
#include "report_envelope.h"
#include "report_details.h"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <format>
#include <vector>

namespace dingosdk::crash_report {
namespace {
struct Internet {
    HINTERNET value{};
    ~Internet() { if (value) WinHttpCloseHandle(value); }
};
std::string utf8(std::wstring_view value) {
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid text");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
void remove_report(const std::filesystem::path& dump) { remove_report_files(dump); }
void status_file(const std::filesystem::path& directory, const std::string& text) {
    // Overwrite a bounded status file. Never include the endpoint, token, response body, or local paths.
    std::ofstream(directory / L"status.txt", std::ios::trunc) << text << '\n';
}
}
bool parse_endpoint(const std::wstring& url, Endpoint& endpoint) {
    if (url.empty() || url.size() >= 4096 || url.find_first_of(L"\r\n\t #") != std::wstring::npos) return false;
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS || !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength ||
        !parts.dwUrlPathLength) return false;
    endpoint.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    endpoint.resource.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) endpoint.resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    endpoint.port = parts.nPort;
    return true;
}
bool accepted_response(DWORD status, std::string_view response, std::string& report_id) {
    report_id.clear();
    if (status < 200 || status >= 300) return false;
    // Sentry's envelope endpoint answers {"id":"<event ID>"}; its minidump endpoint the ID as plain text.
    const auto first = response.find_first_not_of(" \t\r\n"), last = response.find_last_not_of(" \t\r\n");
    if (first == std::string_view::npos) return false;
    auto id = std::string(response.substr(first, last - first + 1));
    if (id.starts_with('{')) {
        try {
            const auto receipt = Json::parse(id, {4096, 4, 64});
            id = receipt.is_object() && receipt.contains("id") && receipt.at("id").is_string() ? receipt.at("id").string() : std::string{};
        } catch (...) { return false; }
    }
    if (id.size() > 36 || id.find_first_not_of("0123456789abcdefABCDEF-") != std::string_view::npos ||
            std::count_if(id.begin(), id.end(), [](char c) { return c != '-'; }) != 32) return false;
    report_id = id;
    return true;
}
std::filesystem::path queue_directory(const std::filesystem::path& root, const std::wstring& url) {
    // Hash the destination so changing projects cannot send an old queue to a new account.
    const auto encoded = utf8(url);
    std::array<unsigned char, 32> hash{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<char*>(encoded.data())), static_cast<ULONG>(encoded.size()),
            hash.data(), static_cast<ULONG>(hash.size())) < 0) throw std::runtime_error("Cannot identify report queue");
    std::wstring name;
    for (const auto byte : hash) name += std::format(L"{:02x}", byte);
    return root / name;
}
UploadResult upload_report(const std::wstring& url, const std::filesystem::path& dump, const Json& attributes) {
    UploadResult result;
    Endpoint endpoint;
    if (!parse_endpoint(url, endpoint) || !attributes.is_object()) { result.error = ERROR_INVALID_PARAMETER; return result; }
    // Sent as an envelope (report_envelope.h), to the envelope endpoint beside the minidump one.
    Endpoint envelope;
    if (!parse_endpoint(envelope_endpoint(url), envelope)) { result.error = ERROR_INVALID_PARAMETER; return result; }
    endpoint = envelope;
    std::array<unsigned char, 16> random{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        result.error = ERROR_INVALID_DATA; return result;
    }
    std::string event_id;
    for (const auto byte : random) event_id += std::format("{:02x}", byte);
    ReportEnvelope body(dump, attributes, event_id);
    if (body.error()) { result.error = body.error(); return result; }
    Internet session{WinHttpOpen(L"ReSkate-CrashReporter/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)};
    if (!session.value) { result.error = GetLastError(); return result; }
    if (!WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000)) { result.error = GetLastError(); return result; }
    Internet connection{WinHttpConnect(session.value, endpoint.host.c_str(), endpoint.port, 0)};
    if (!connection.value) { result.error = GetLastError(); return result; }
    Internet request{WinHttpOpenRequest(connection.value, L"POST", endpoint.resource.c_str(), nullptr, nullptr,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.value) { result.error = GetLastError(); return result; }
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER, logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon))) {
        result.error = GetLastError(); return result;
    }
    const std::wstring headers = L"Content-Type: application/x-sentry-envelope\r\n";
    if (!WinHttpSendRequest(request.value, headers.c_str(), static_cast<DWORD>(headers.size()), nullptr, 0, body.size(), 0)) {
        result.error = GetLastError(); return result;
    }
    const auto started = GetTickCount64();
    const auto write = [&](const void* data, DWORD bytes) {
        DWORD written{};
        return GetTickCount64() - started < 60000 && WinHttpWriteData(request.value, data, bytes, &written) && written == bytes;
    };
    if (!body.write(write)) { result.error = ERROR_WRITE_FAULT; return result; }
    if (!WinHttpReceiveResponse(request.value, nullptr)) {
        result.error = GetLastError(); return result;
    }
    DWORD status_size = sizeof(result.http_status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
            &result.http_status, &status_size, nullptr)) { result.error = GetLastError(); return result; }
    std::string response;
    std::array<char, 65536> buffer{};
    for (;;) {
        DWORD read{};
        if (GetTickCount64() - started >= 60000 || !WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) {
            result.error = ERROR_TIMEOUT; return result;
        }
        if (!read) break;
        if (response.size() + read > 65536) { result.error = ERROR_INVALID_DATA; return result; }
        response.append(buffer.data(), read);
    }
    result.accepted = accepted_response(result.http_status, response, result.report_id);
    return result;
}
std::size_t upload_pending(const std::wstring& url, const std::filesystem::path& directory, const Sender& sender) noexcept {
    try {
        Handle lock(CreateFileW((directory / L"queue.lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (lock.value == INVALID_HANDLE_VALUE) return 0;
        std::vector<std::filesystem::directory_entry> pending;
        const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 14);
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (!entry.is_regular_file() || entry.path().extension() != L".dmp") continue;
            if (entry.last_write_time() < cutoff || entry.file_size() > max_dump_bytes || !entry.file_size()) remove_report(entry.path());
            else pending.push_back(entry);
        }
        std::sort(pending.begin(), pending.end(), [](const auto& a, const auto& b) { return a.last_write_time() < b.last_write_time(); });
        while (pending.size() > max_pending_reports) { remove_report(pending.front().path()); pending.erase(pending.begin()); }
        std::size_t uploaded{};
        for (const auto& entry : pending) {
            const auto metadata = entry.path().wstring() + L".json";
            std::error_code error;
            const auto size = std::filesystem::file_size(metadata, error);
            if (error || size > 65536) continue;
            std::ifstream input(metadata, std::ios::binary);
            std::string contents(static_cast<std::size_t>(size), '\0');
            if (!input.read(contents.data(), static_cast<std::streamsize>(contents.size()))) continue;
            input.close(); // Windows cannot remove an open metadata file after confirmation.
            Json attributes;
            try { attributes = Json::parse(contents, {65536, 8, 256}); } catch (...) { continue; }
            const auto result = sender(url, entry.path(), attributes);
            if (!result.accepted) {
                status_file(directory, std::format("Upload pending; HTTP {}; Windows error {}. Retrying on next launch.", result.http_status, result.error));
                break;
            }
            status_file(directory, "Accepted by Sentry: " + result.report_id);
            keep_sent_report(entry.path());
            if (++uploaded == 4) break; // Bound work for a single launch.
        }
        return uploaded;
    } catch (...) { return 0; }
}
}
