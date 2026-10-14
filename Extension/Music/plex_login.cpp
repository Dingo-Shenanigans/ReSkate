#include "plex_login.h"
#include "Engine/Core/Json/json.h"
#include "Extension/Boot/user_data_redirect.h"
#include <Windows.h>
#include <dpapi.h>
#include <objbase.h>
#include <winhttp.h>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>

namespace dingosdk::plex_login {
namespace {
std::mutex mutex;
std::string client_id, access_token, shown_code, problem;
std::atomic<bool> pending{};
std::atomic<unsigned> attempt{}; // a newer try, or a sign-out, ends the one waiting
bool loaded{};

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    std::wstring out(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
    return out;
}
std::string sealed(const std::string& text) {
    DATA_BLOB in{static_cast<DWORD>(text.size()), reinterpret_cast<BYTE*>(const_cast<char*>(text.data()))}, out{};
    if (text.empty() || !CryptProtectData(&in, L"ReSkate Plex", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string hex;
    for (DWORD i = 0; i < out.cbData; ++i) hex += std::format("{:02x}", out.pbData[i]);
    LocalFree(out.pbData);
    return hex;
}
std::string unsealed(const std::string& hex) {
    std::vector<BYTE> bytes;
    const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0; };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(static_cast<BYTE>(digit(hex[i]) * 16 + digit(hex[i + 1])));
    DATA_BLOB in{static_cast<DWORD>(bytes.size()), bytes.data()}, out{};
    if (bytes.empty() || !CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    std::string text(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return text;
}
std::filesystem::path kept_file() { return redirected_local_app_data().parent_path() / L"plex.json"; }
void keep() {
    try {
        Json json = Json::object();
        {
            std::lock_guard lock(mutex);
            json["client_id"] = client_id;
            json["token"] = sealed(access_token);
        }
        std::error_code ignored;
        std::filesystem::create_directories(kept_file().parent_path(), ignored);
        std::ofstream(kept_file(), std::ios::binary | std::ios::trunc) << json.dump(2);
    } catch (...) {}
}
// Read once; makes the id that names this player to Plex if there is none.
void load() {
    std::lock_guard lock(mutex);
    if (loaded) return;
    loaded = true;
    try {
        std::ifstream in(kept_file(), std::ios::binary);
        if (in) {
            const auto json = Json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
            if (json.contains("client_id") && json.at("client_id").is_string()) client_id = json.at("client_id").get<std::string>();
            if (json.contains("token") && json.at("token").is_string()) access_token = unsealed(json.at("token").get<std::string>());
        }
    } catch (...) {}
    if (client_id.empty()) {
        GUID guid{};
        if (SUCCEEDED(CoCreateGuid(&guid)))
            client_id = std::format("reskate-{:08x}{:04x}{:04x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
                guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
        else client_id = "reskate-music";
    }
}
std::string id() {
    load();
    std::lock_guard lock(mutex);
    return client_id;
}

struct Handle {
    HINTERNET h{};
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};
Reply call(const wchar_t* method, const std::string& host, unsigned short port, bool secure, const std::string& path, const std::wstring& headers, std::size_t largest) {
    Reply reply;
    Handle session{WinHttpOpen(L"ReSkate", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) return reply;
    WinHttpSetTimeouts(session.h, 5000, 5000, 8000, 10000);
    Handle connection{WinHttpConnect(session.h, wide(host).c_str(), port, 0)};
    Handle request{connection.h ? WinHttpOpenRequest(connection.h, method, wide(path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr};
    if (!request.h || !WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return reply;
    DWORD status{}, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    reply.status = status;
    char buffer[8192];
    for (DWORD got{}; WinHttpReadData(request.h, buffer, sizeof(buffer), &got) && got;) {
        reply.body.append(buffer, got);
        if (reply.body.size() > largest) {
            reply.status = 0;
            reply.body.clear();
            break;
        }
    }
    return reply;
}
std::wstring plex_headers() {
    return L"Accept: application/json\r\nX-Plex-Product: ReSkate\r\nX-Plex-Version: 1\r\nX-Plex-Device-Name: ReSkate\r\nX-Plex-Client-Identifier: " + wide(id()) + L"\r\n";
}

void wait_for_code(unsigned mine, std::uint64_t pin, std::string code) noexcept {
    try {
        for (int i = 0; i < 450 && attempt.load() == mine; ++i) { // (15 minutes, as Plex's code lasts)
            Sleep(2000);
            if (attempt.load() != mine) return;
            const auto reply = call(L"GET", "plex.tv", 443, true, std::format("/api/v2/pins/{}?code={}", pin, code), plex_headers(), 64 * 1024);
            if (reply.status == 404) break;
            if (reply.status != 200) continue;
            const auto json = Json::parse(reply.body);
            if (json.contains("authToken") && json.at("authToken").is_string() && !json.at("authToken").get<std::string>().empty()) {
                {
                    std::lock_guard lock(mutex);
                    access_token = json.at("authToken").get<std::string>();
                    shown_code.clear();
                    problem.clear();
                }
                keep();
                pending.store(false);
                return;
            }
        }
        if (attempt.load() == mine) {
            std::lock_guard lock(mutex);
            shown_code.clear();
            problem = "The code ran out before it was entered. Try again.";
        }
    } catch (...) {}
    if (attempt.load() == mine) pending.store(false);
}
} // namespace

Reply get(const std::string& host, unsigned short port, bool secure, const std::string& path, std::size_t largest) {
    try {
        return call(L"GET", host, port, secure, path, L"", largest);
    } catch (...) {
        return {};
    }
}
State state() {
    load();
    State out;
    std::lock_guard lock(mutex);
    out.signed_in = !access_token.empty();
    out.pending = pending.load();
    out.code = shown_code;
    out.problem = problem;
    return out;
}
std::string token() {
    load();
    std::lock_guard lock(mutex);
    return access_token;
}
void begin() {
    const auto mine = attempt.fetch_add(1) + 1;
    try {
        const auto reply = call(L"POST", "plex.tv", 443, true, "/api/v2/pins?strong=false", plex_headers(), 64 * 1024);
        const auto json = reply.status == 200 || reply.status == 201 ? Json::parse(reply.body) : Json::object();
        if (!json.contains("id") || !json.at("id").is_number() || !json.contains("code") || !json.at("code").is_string()) {
            std::lock_guard lock(mutex);
            problem = "Could not reach plex.tv.";
            pending.store(false);
            return;
        }
        const auto pin = json.at("id").get<std::uint64_t>();
        auto code = json.at("code").get<std::string>();
        {
            std::lock_guard lock(mutex);
            shown_code = code;
            problem.clear();
        }
        pending.store(true);
        std::thread(wait_for_code, mine, pin, std::move(code)).detach();
    } catch (...) {
        std::lock_guard lock(mutex);
        problem = "Could not reach plex.tv.";
        pending.store(false);
    }
}
void sign_out() {
    attempt.fetch_add(1);
    pending.store(false);
    {
        load();
        std::lock_guard lock(mutex);
        access_token.clear();
        shown_code.clear();
        problem.clear();
    }
    keep();
}
}
