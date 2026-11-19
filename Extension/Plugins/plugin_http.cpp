#include "plugin_http.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace dingosdk::plugins::http {
namespace {
constexpr int most_going = 16;
constexpr int most_redirects = 5;

// A request going on: how far its answer has come, and whether it was given up.
struct Going {
    std::atomic<std::uint64_t> received{}, total{};
    std::atomic<bool> cancelled{};
};
std::mutex mutex;
std::vector<HttpResponse> answers;                                             // under mutex
std::map<std::pair<std::string, std::uint64_t>, std::shared_ptr<Going>> live;  // under mutex: by plugin and id
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

struct Handle {
    HINTERNET value{};
    ~Handle() { if (value) WinHttpCloseHandle(value); }
};
// One header of the answer as text, or empty.
std::string header(HINTERNET call, DWORD which) {
    DWORD size = 0;
    WinHttpQueryHeaders(call, which, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !size) return {};
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(call, which, WINHTTP_HEADER_NAME_BY_INDEX, text.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    text.resize(size / sizeof(wchar_t));
    std::string out;
    for (const wchar_t c : text) out += c < 0x80 ? static_cast<char>(c) : '?';
    return out;
}
// The answer's headers, names in lower case, from WinHTTP's "Name: value" lines (the status line left out).
std::vector<std::pair<std::string, std::string>> headers_of(HINTERNET call) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto raw = header(call, WINHTTP_QUERY_RAW_HEADERS_CRLF);
    std::size_t at = 0;
    while (at < raw.size() && out.size() < 64) {
        const auto end = std::min(raw.find("\r\n", at), raw.size());
        const auto line = std::string_view(raw).substr(at, end - at);
        at = end + 2;
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) continue;
        std::string name(line.substr(0, colon));
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        auto value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        out.emplace_back(std::move(name), std::string(value.substr(0, 4096)));
    }
    return out;
}
// "https://<host>[:443]/<path>" split; false for anything else.
bool split_url(std::string_view url, std::string& host, std::string& path) {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme)) return false;
    const auto rest = url.substr(scheme.size());
    const auto slash = rest.find_first_of("/?");
    host = std::string(rest.substr(0, slash));
    path = slash == std::string_view::npos ? std::string("/") : std::string(rest.substr(slash));
    if (!path.starts_with('/')) path.insert(path.begin(), '/');
    if (host.ends_with(":443")) host.resize(host.size() - 4);
    for (auto& c : host) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    if (host.empty() || host.find_first_of("@:[]") != std::string::npos) return false;
    for (const char c : url)
        if (c <= 0x20 || c > 0x7e) return false;
    return true;
}

HttpResponse run(const HttpRequest& request, Going& state) {
    HttpResponse response{request.id, request.plugin};
    const auto agent = wide("ReSkate-Plugin/" + request.plugin);
    Handle session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.value) session.value = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value) {
        response.error = "web requests are unavailable";
        return response;
    }
    const int timeout = request.timeout_ms;
    WinHttpSetTimeouts(session.value, timeout, timeout, timeout, timeout);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    // An answer that goes to a file is written beside it as "<file>.part", with the server's name for
    // what it is sending (its ETag) in "<file>.part.etag": a request that broke off is gone on with
    // from there when the server still has the same file, and starts again when it has another.
    const bool to_file = !request.to_file.empty();
    auto part = request.to_file, tag = request.to_file;
    part += L".part";
    tag += L".part.etag";
    std::string host = request.host, path = request.path;
    std::string method = request.method;
    bool own_host = true; // still the host the plugin asked: its own headers are sent
    for (int hop = 0;; ++hop) {
        if (auto error = check_host(host); !error.empty()) {
            response.error = error;
            return response;
        }
        Handle connection{WinHttpConnect(session.value, wide(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0)};
        if (!connection.value) {
            response.error = "it could not connect";
            return response;
        }
        Handle call{WinHttpOpenRequest(connection.value, wide(method).c_str(), wide(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
        if (!call.value) {
            response.error = "it could not be made";
            return response;
        }
        // Redirects are never WinHTTP's to follow: each is looked at here first.
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(call.value, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));
        DWORD off = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_KEEP_ALIVE;
        WinHttpSetOption(call.value, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof(off));
        DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        WinHttpSetOption(call.value, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon));
        std::wstring headers;
        const bool sends_body = method != "GET" && method != "HEAD";
        if (sends_body && !request.content_type.empty()) headers = L"Content-Type: " + wide(request.content_type) + L"\r\n";
        if (own_host)
            for (const auto& [name, value] : request.headers) headers += wide(name) + L": " + wide(value) + L"\r\n";
        // Going on with a part: from its size, if the server's file is still the one the part came from.
        std::uint64_t have = 0;
        if (to_file && request.resume && method == "GET") {
            std::error_code code;
            const auto size = std::filesystem::file_size(part, code);
            std::string etag;
            if (!code && size > 0 && size < request.most) {
                std::ifstream in(tag, std::ios::binary);
                std::getline(in, etag);
            }
            const bool plain = !etag.empty() && etag.size() < 256 &&
                               std::all_of(etag.begin(), etag.end(), [](char c) { return c > 0x20 && c < 0x7f; });
            if (plain) {
                have = size;
                headers += L"Range: bytes=" + std::to_wstring(have) + L"-\r\nIf-Range: " + wide(etag) + L"\r\n";
            }
        }
        const auto* body = sends_body && !request.body.empty() ? request.body.data() : nullptr;
        const auto body_size = body ? static_cast<DWORD>(request.body.size()) : 0;
        const BOOL sent = WinHttpSendRequest(call.value, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                             headers.empty() ? 0 : static_cast<DWORD>(-1L), body ? const_cast<char*>(body) : WINHTTP_NO_REQUEST_DATA,
                                             body_size, body_size, 0);
        if (!sent || !WinHttpReceiveResponse(call.value, nullptr)) {
            response.error = GetLastError() == ERROR_WINHTTP_TIMEOUT ? "it took too long" : "it failed (no answer, or its certificate was not valid)";
            return response;
        }
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(call.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        response.status = static_cast<int>(status);
        response.headers = headers_of(call.value);
        // A redirect: followed only when asked, only over HTTPS and only to a host the manifest lists.
        if (request.follow && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
            const auto location = header(call.value, WINHTTP_QUERY_LOCATION);
            std::string next_host, next_path;
            if (location.starts_with('/')) next_host = host, next_path = location;
            else if (!split_url(location, next_host, next_path)) {
                response.error = "it was sent on to an address that is not HTTPS";
                return response;
            }
            if (std::find(request.domains.begin(), request.domains.end(), next_host) == request.domains.end()) {
                response.error = "it was sent on to " + next_host + ", which is not in the plugin's domains";
                return response;
            }
            if (hop >= most_redirects) {
                response.error = "it was sent on more than 5 times";
                return response;
            }
            // (What a plugin put in its headers goes to the host it asked, never on to another.)
            if (next_host != request.host) own_host = false;
            if (status == 303 || ((status == 301 || status == 302) && method == "POST")) method = "GET";
            host = std::move(next_host);
            path = std::move(next_path);
            continue;
        }
        // The body: into memory, or into the part file.
        std::ofstream out;
        if (to_file && status >= 200 && status < 300) {
            std::error_code code;
            std::filesystem::create_directories(part.parent_path(), code);
            const bool appending = status == 206 && have > 0;
            if (!appending) have = 0;
            out.open(part, std::ios::binary | (appending ? std::ios::app : std::ios::trunc));
            if (!out) {
                response.error = "the file cannot be written";
                return response;
            }
            // What this file is, for a later request to go on with.
            std::string etag;
            for (const auto& [name, value] : response.headers)
                if (name == "etag") etag = value;
            std::ofstream(tag, std::ios::binary | std::ios::trunc) << etag;
        }
        std::uint64_t length = 0;
        for (const auto& [name, value] : response.headers)
            if (name == "content-length") length = std::strtoull(value.c_str(), nullptr, 10);
        state.received.store(have);
        state.total.store(length ? have + length : 0);
        std::uint64_t got = have;
        std::string chunk;
        for (;;) {
            if (state.cancelled.load()) {
                response.error = "it was given up";
                response.body.clear();
                break;
            }
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(call.value, &available)) {
                response.error = GetLastError() == ERROR_WINHTTP_TIMEOUT ? "it took too long" : "its answer broke off";
                break;
            }
            if (!available) break;
            if (got + available > request.most) {
                response.error = "its answer is larger than " + std::to_string(std::max<std::uint64_t>(1, request.most >> 20)) + " MB";
                response.body.clear();
                break;
            }
            chunk.resize(available);
            DWORD read = 0;
            if (!WinHttpReadData(call.value, chunk.data(), available, &read)) {
                response.error = "its answer broke off";
                break;
            }
            if (out.is_open()) {
                if (!out.write(chunk.data(), read)) {
                    response.error = "the file cannot be written";
                    break;
                }
            } else response.body.append(chunk.data(), read);
            got += read;
            state.received.store(got);
        }
        if (out.is_open()) {
            out.close();
            response.body.clear();
            if (response.error.empty()) {
                // Whole: into place, in place of what was there.
                if (!MoveFileExW(part.c_str(), request.to_file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    response.error = "the file cannot be written";
                std::error_code code;
                std::filesystem::remove(tag, code);
            } else if (!request.resume || response.error.starts_with("its answer is larger")) {
                // (Not to be gone on with: the part goes.)
                std::error_code code;
                std::filesystem::remove(part, code);
                std::filesystem::remove(tag, code);
            }
        }
        return response;
    }
}
}

bool start(HttpRequest request) noexcept {
    if (going.fetch_add(1) >= most_going) {
        going.fetch_sub(1);
        return false;
    }
    try {
        auto state = std::make_shared<Going>();
        {
            std::lock_guard lock(mutex);
            live[{request.plugin, request.id}] = state;
        }
        std::thread([request = std::move(request), state] {
            HttpResponse response;
            try {
                response = run(request, *state);
            } catch (const std::exception& error) {
                response = {request.id, request.plugin, 0, {}, error.what()};
            }
            {
                std::lock_guard lock(mutex);
                live.erase({request.plugin, request.id});
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

std::optional<HttpProgress> progress(std::string_view plugin, std::uint64_t id) noexcept {
    try {
        std::lock_guard lock(mutex);
        const auto found = live.find({std::string(plugin), id});
        if (found == live.end()) return std::nullopt;
        return HttpProgress{found->second->received.load(), found->second->total.load()};
    } catch (...) {
        return std::nullopt;
    }
}

void cancel(std::string_view plugin, std::uint64_t id) noexcept {
    try {
        std::lock_guard lock(mutex);
        if (const auto found = live.find({std::string(plugin), id}); found != live.end()) found->second->cancelled.store(true);
    } catch (...) {
    }
}
}
