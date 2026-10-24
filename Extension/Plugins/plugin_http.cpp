#include "plugin_http.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <winhttp.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

namespace dingosdk::plugins::http {
namespace {
constexpr std::size_t most_bytes = 1u << 20;
constexpr DWORD timeout_ms = 10000;
constexpr int most_going = 8;

std::mutex mutex;
std::vector<HttpResponse> answers; // under mutex
std::atomic<int> going{};

std::wstring wide(const std::string& text) { return std::wstring(text.begin(), text.end()); } // ASCII, checked by the API

// Loopback, private, link-local, shared (CGNAT), multicast, reserved and unspecified addresses:
// a plugin's request never reaches the player's own network.
bool reserved(const sockaddr* address) {
    if (address->sa_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        const auto ip = ntohl(v4->sin_addr.s_addr);
        const auto a = ip >> 24, b = (ip >> 16) & 0xff;
        return a == 0 || a == 10 || a == 127 || (a == 100 && b >= 64 && b < 128) || (a == 169 && b == 254) ||
               (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 192 && b == 0) || (a == 198 && (b == 18 || b == 19)) ||
               a >= 224;
    }
    if (address->sa_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
        const auto* bytes = v6->sin6_addr.s6_addr;
        bool zero = true;
        for (int i = 0; i < 15; ++i) zero = zero && bytes[i] == 0;
        if (zero && (bytes[15] == 0 || bytes[15] == 1)) return true;                  // :: and ::1
        if ((bytes[0] & 0xfe) == 0xfc) return true;                                  // fc00::/7
        if (bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80) return true;              // fe80::/10
        if (bytes[0] == 0xff) return true;                                           // multicast
        bool mapped = true;
        for (int i = 0; i < 10; ++i) mapped = mapped && bytes[i] == 0;
        if (mapped && bytes[10] == 0xff && bytes[11] == 0xff) {                      // ::ffff:a.b.c.d
            sockaddr_in v4{};
            v4.sin_family = AF_INET;
            std::memcpy(&v4.sin_addr, bytes + 12, 4);
            return reserved(reinterpret_cast<const sockaddr*>(&v4));
        }
        return false;
    }
    return true;
}
// Why the host may not be reached, or empty.
std::string check_host(const std::string& host) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return "the network is unavailable";
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    std::string error;
    if (getaddrinfo(host.c_str(), "443", &hints, &found) != 0 || !found) error = "its host name could not be found";
    for (auto* item = found; item && error.empty(); item = item->ai_next)
        if (reserved(item->ai_addr)) error = "its host is on a private or local network";
    if (found) freeaddrinfo(found);
    WSACleanup();
    return error;
}

HttpResponse run(const HttpRequest& request) {
    HttpResponse response{request.id, request.plugin};
    if (auto error = check_host(request.host); !error.empty()) {
        response.error = error;
        return response;
    }
    struct Handle {
        HINTERNET value{};
        ~Handle() { if (value) WinHttpCloseHandle(value); }
    };
    const auto agent = wide("ReSkate-Plugin/" + request.plugin);
    Handle session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value) session.value = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) {
        response.error = "web requests are unavailable";
        return response;
    }
    WinHttpSetTimeouts(session.value, static_cast<int>(timeout_ms), static_cast<int>(timeout_ms), static_cast<int>(timeout_ms), static_cast<int>(timeout_ms));
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    Handle connection{WinHttpConnect(session.value, wide(request.host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.value) {
        response.error = "it could not connect";
        return response;
    }
    Handle call{WinHttpOpenRequest(connection.value, wide(request.method).c_str(), wide(request.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!call.value) {
        response.error = "it could not be made";
        return response;
    }
    DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(call.value, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));
    DWORD off = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_KEEP_ALIVE;
    WinHttpSetOption(call.value, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    WinHttpSetOption(call.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon));
    std::wstring headers;
    if (!request.content_type.empty()) headers = L"Content-Type: " + wide(request.content_type) + L"\r\n";
    const BOOL sent = WinHttpSendRequest(call.value, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                         headers.empty() ? 0 : static_cast<DWORD>(-1L),
                                         request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data()),
                                         static_cast<DWORD>(request.body.size()), static_cast<DWORD>(request.body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(call.value, nullptr)) {
        response.error = GetLastError() == ERROR_WINHTTP_TIMEOUT ? "it took longer than 10 s" : "it failed (no answer, or its certificate was not valid)";
        return response;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(call.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(call.value, &available)) {
            response.error = "its answer broke off";
            break;
        }
        if (!available) break;
        if (response.body.size() + available > most_bytes) {
            response.error = "its answer is larger than 1 MB";
            response.body.clear();
            break;
        }
        const auto at = response.body.size();
        response.body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(call.value, response.body.data() + at, available, &read)) {
            response.error = "its answer broke off";
            break;
        }
        response.body.resize(at + read);
    }
    return response;
}
}

bool start(HttpRequest request) noexcept {
    if (going.fetch_add(1) >= most_going) {
        going.fetch_sub(1);
        return false;
    }
    try {
        std::thread([request = std::move(request)] {
            HttpResponse response;
            try {
                response = run(request);
            } catch (const std::exception& error) {
                response = {request.id, request.plugin, 0, {}, error.what()};
            }
            {
                std::lock_guard lock(mutex);
                if (answers.size() < 256) answers.push_back(std::move(response));
            }
            going.fetch_sub(1);
        }).detach();
        return true;
    } catch (...) {
        going.fetch_sub(1);
        return false;
    }
}

std::vector<HttpResponse> take() noexcept {
    std::lock_guard lock(mutex);
    return std::exchange(answers, {});
}
}
