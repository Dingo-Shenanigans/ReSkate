#include "plex_web.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Boot/user_data_redirect.h"
#include <Windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <shellapi.h>
#include <winhttp.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp")
#pragma comment(lib, "bcrypt")
#pragma comment(lib, "crypt32")
#pragma comment(lib, "shell32")

namespace dingosdk::plex_web {
namespace {
constexpr wchar_t agent[] = L"ReSkate";
constexpr wchar_t product[] = L"ReSkate";
constexpr std::uint64_t approval_wait_ms = 5 * 60 * 1000;

std::mutex mutex; // everything below it
std::condition_variable wake;
std::deque<std::string> commands; // "login", "logout"
std::string auth_token, client_id, user_name, problem_text;
bool is_busy{};
std::atomic<bool> started{};

void say(logging::Level level, const std::string& text) noexcept {
    try {
        logging::log(level, logging::Channel::music, "Plex account: {}", text);
    } catch (...) {}
}
void problem(const std::string& text) {
    {
        std::lock_guard lock(mutex);
        problem_text = text;
    }
    if (!text.empty()) say(logging::Level::warning, text);
}

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    std::wstring out(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
    return out;
}
std::string escaped(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(static_cast<char>(c));
        else out += std::format("%{:02X}", c);
    }
    return out;
}
std::string random_hex(std::size_t bytes) {
    std::vector<unsigned char> data(bytes);
    if (BCryptGenRandom(nullptr, data.data(), static_cast<ULONG>(data.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    std::string out;
    for (const auto b : data) out += std::format("{:02x}", b);
    return out;
}

// The token, kept so that only this Windows account can read it.
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
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0; };
        bytes.push_back(static_cast<BYTE>(digit(hex[i]) * 16 + digit(hex[i + 1])));
    }
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
            json["token"] = sealed(auth_token);
            json["user"] = user_name;
        }
        std::error_code ignored;
        std::filesystem::create_directories(kept_file().parent_path(), ignored);
        std::ofstream(kept_file(), std::ios::binary | std::ios::trunc) << json.dump(2);
    } catch (...) {}
}
void read_kept() {
    try {
        std::ifstream in(kept_file(), std::ios::binary);
        if (in) {
            const auto json = Json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
            std::lock_guard lock(mutex);
            if (json.contains("client_id") && json.at("client_id").is_string()) client_id = json.at("client_id").string();
            if (json.contains("token") && json.at("token").is_string()) auth_token = unsealed(json.at("token").string());
            if (json.contains("user") && json.at("user").is_string()) user_name = json.at("user").string();
        }
    } catch (...) {}
    bool fresh{};
    {
        std::lock_guard lock(mutex);
        if (client_id.empty()) {
            client_id = "reskate-" + random_hex(12);
            fresh = true;
        }
    }
    if (fresh) keep();
}

struct Reply {
    unsigned long status{};
    std::string body;
};
struct Handle {
    HINTERNET h{};
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};
// A call to plex.tv or to the player's server, as Plex's API asks for one: JSON back, and who is asking in its headers.
Reply request(const wchar_t* method, const std::wstring& host, INTERNET_PORT port, bool secure, const std::wstring& path, const std::string& token = {}, const std::wstring& extra = {}) {
    Reply reply;
    std::wstring headers = L"Accept: application/json\r\nX-Plex-Product: " + std::wstring(product) + L"\r\nX-Plex-Client-Identifier: " + wide(client_identifier()) +
        L"\r\nX-Plex-Device-Name: ReSkate\r\nX-Plex-Platform: Windows\r\n" + extra;
    if (!token.empty()) headers += L"X-Plex-Token: " + wide(token) + L"\r\n";
    Handle session{WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) return reply;
    WinHttpSetTimeouts(session.h, 5000, 5000, 10000, 10000);
    Handle connection{WinHttpConnect(session.h, host.c_str(), port, 0)};
    Handle call{connection.h ? WinHttpOpenRequest(connection.h, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) : nullptr};
    if (!call.h || !WinHttpSendRequest(call.h, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(call.h, nullptr)) return reply;
    DWORD status{}, size = sizeof(status);
    WinHttpQueryHeaders(call.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    reply.status = status;
    char buffer[8192];
    for (DWORD got{}; WinHttpReadData(call.h, buffer, sizeof(buffer), &got) && got && reply.body.size() < 4 * 1024 * 1024;) reply.body.append(buffer, got);
    return reply;
}
Reply request(const wchar_t* method, const std::wstring& path, const std::string& token = {}) {
    return request(method, L"plex.tv", INTERNET_DEFAULT_HTTPS_PORT, true, path, token);
}

// Who the token belongs to. False when plex.tv refuses it (true, with no name, when it could not be asked).
bool check_token(const std::string& token, std::string& name, bool& refused) {
    refused = false;
    const auto reply = request(L"GET", L"/api/v2/user", token);
    if (reply.status == 401 || reply.status == 403) {
        refused = true;
        return false;
    }
    if (reply.status != 200) return false;
    try {
        const auto json = Json::parse(reply.body);
        for (const auto* key : {"username", "title", "email"})
            if (json.contains(key) && json.at(key).is_string() && !json.at(key).string().empty()) {
                name = json.at(key).string();
                break;
            }
    } catch (...) {}
    return true;
}

void sign_in() {
    problem({});
    const auto made = request(L"POST", L"/api/v2/pins?strong=true");
    std::uint64_t id{};
    std::string code;
    try {
        const auto json = Json::parse(made.body);
        if ((made.status != 200 && made.status != 201) || !json.contains("id") || !json.contains("code")) return problem(std::format("Plex would not start a sign-in ({})", made.status));
        id = json.at("id").get<std::uint64_t>();
        code = json.at("code").string();
    } catch (...) {
        return problem("Plex would not start a sign-in");
    }
    const auto address = "https://app.plex.tv/auth#?clientID=" + escaped(client_identifier()) + "&code=" + escaped(code) + "&context%5Bdevice%5D%5Bproduct%5D=ReSkate";
    ShellExecuteW(nullptr, L"open", wide(address).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    say(logging::Level::info, "waiting for the sign-in in the browser.");
    const auto until = GetTickCount64() + approval_wait_ms;
    std::string granted;
    while (GetTickCount64() < until) {
        Sleep(2000);
        const auto polled = request(L"GET", L"/api/v2/pins/" + std::to_wstring(id) + L"?code=" + wide(escaped(code)));
        if (polled.status == 404) return problem("the sign-in expired; try again");
        if (polled.status != 200) continue;
        try {
            const auto json = Json::parse(polled.body);
            if (json.contains("authToken") && json.at("authToken").is_string() && !json.at("authToken").string().empty()) {
                granted = json.at("authToken").string();
                break;
            }
        } catch (...) {}
    }
    if (granted.empty()) return problem("the sign-in was not finished in the browser");
    std::string name;
    bool refused{};
    check_token(granted, name, refused);
    {
        std::lock_guard lock(mutex);
        auth_token = granted;
        user_name = name;
        problem_text.clear();
    }
    keep();
    say(logging::Level::info, "signed in.");
}

// The player's server, found through plex.tv (worker's alone, but for `session_now`).
struct Server {
    std::wstring host;
    INTERNET_PORT port{};
    bool secure{};
    std::string token;
};
Server server;
bool have_server{};
std::uint64_t server_tried_at{};
std::uint64_t polled_at{};
std::string art_thumb, session_client;
std::atomic<std::uint64_t> wanted_at{};
NowPlaying session_now;

std::uint64_t tick() { return GetTickCount64(); }

bool parse_uri(const std::string& uri, Server& out) {
    const auto scheme = uri.find("://");
    if (scheme == std::string::npos) return false;
    out.secure = uri.compare(0, scheme, "https") == 0;
    auto rest = uri.substr(scheme + 3);
    if (const auto slash = rest.find('/'); slash != std::string::npos) rest.resize(slash);
    unsigned port = out.secure ? 443 : 80;
    if (const auto colon = rest.rfind(':'); colon != std::string::npos) {
        try {
            port = static_cast<unsigned>(std::stoul(rest.substr(colon + 1)));
        } catch (...) {
            return false;
        }
        rest.resize(colon);
    }
    if (rest.empty() || port == 0 || port > 65535) return false;
    out.host = wide(rest);
    out.port = static_cast<INTERNET_PORT>(port);
    return true;
}
Reply to_server(const wchar_t* method, const std::string& path, const std::wstring& extra = {}) {
    return request(method, server.host, server.port, server.secure, wide(path), server.token, extra);
}

// Looks for a server the account owns that answers: its local address first, then the others, a relay last.
bool find_server() {
    server_tried_at = tick();
    const auto reply = request(L"GET", L"/api/v2/resources?includeHttps=1&includeRelay=1", token());
    if (reply.status != 200) return false;
    try {
        const auto json = Json::parse(reply.body);
        for (int pass = 0; pass < 3; ++pass)
            for (std::size_t i = 0; i < json.size(); ++i) {
                const auto& resource = json.at(i);
                if (!resource.contains("provides") || resource.at("provides").string().find("server") == std::string::npos) continue;
                if (!resource.contains("connections")) continue;
                const auto access = resource.contains("accessToken") && resource.at("accessToken").is_string() ? resource.at("accessToken").string() : token();
                const auto& connections = resource.at("connections");
                for (std::size_t c = 0; c < connections.size(); ++c) {
                    const auto& connection = connections.at(c);
                    const bool local = connection.contains("local") && connection.at("local").get<bool>();
                    const bool relay = connection.contains("relay") && connection.at("relay").get<bool>();
                    if (pass != (local ? 0 : relay ? 2 : 1)) continue;
                    Server candidate;
                    candidate.token = access;
                    if (!connection.contains("uri") || !parse_uri(connection.at("uri").string(), candidate)) continue;
                    if (request(L"GET", candidate.host, candidate.port, candidate.secure, L"/identity", access).status != 200) continue;
                    server = std::move(candidate);
                    have_server = true;
                    say(logging::Level::info, "found the Plex server.");
                    return true;
                }
            }
    } catch (...) {}
    return false;
}

void poll() {
    if (token().empty()) return;
    if (!have_server && (tick() - server_tried_at < 30000 || !find_server())) return;
    const auto reply = to_server(L"GET", "/status/sessions");
    if (reply.status != 200) {
        have_server = false; // found again next time
        return;
    }
    std::string name;
    {
        std::lock_guard lock(mutex);
        name = user_name;
    }
    NowPlaying found;
    std::string thumb, client;
    try {
        const auto json = Json::parse(reply.body);
        if (json.contains("MediaContainer") && json.at("MediaContainer").contains("Metadata")) {
            const auto& list = json.at("MediaContainer").at("Metadata");
            int matches{}, chosen{-1};
            std::size_t music{};
            int only{-1};
            for (std::size_t i = 0; i < list.size(); ++i) {
                const auto& item = list.at(i);
                if (item.value("type", "") != "track") continue;
                ++music;
                only = static_cast<int>(i);
                const auto who = item.contains("User") ? item.at("User").value("title", "") : std::string();
                if (!name.empty() && _stricmp(who.c_str(), name.c_str()) == 0 && matches++ == 0) chosen = static_cast<int>(i);
            }
            if (chosen < 0 && music == 1) chosen = only;
            if (chosen >= 0) {
                const auto& item = list.at(static_cast<std::size_t>(chosen));
                found.valid = true;
                found.title = item.value("title", "");
                found.artist = item.value("grandparentTitle", "");
                found.album = item.value("parentTitle", "");
                for (const auto* key : {"parentThumb", "thumb", "grandparentThumb"})
                    if (item.contains(key) && item.at(key).is_string() && !item.at(key).string().empty()) {
                        thumb = item.at(key).string();
                        break;
                    }
                if (item.contains("Player")) {
                    found.playing = item.at("Player").value("state", "") == "playing";
                    client = item.at("Player").value("machineIdentifier", "");
                }
            }
        }
    } catch (...) {}
    std::vector<std::uint8_t> art;
    if (found.valid && !thumb.empty() && thumb != art_thumb) {
        const auto picture = to_server(L"GET", "/photo/:/transcode?width=600&height=600&minSize=1&upscale=1&url=" + escaped(thumb));
        if (picture.status == 200 && !picture.body.empty()) {
            art.assign(picture.body.begin(), picture.body.end());
            art_thumb = thumb;
        }
    } else if (!found.valid) {
        art_thumb.clear();
    }
    std::lock_guard lock(mutex);
    found.art = session_now.art;
    found.art_revision = session_now.art_revision;
    if (!art.empty()) {
        found.art = std::move(art);
        ++found.art_revision;
    } else if (!found.valid || thumb.empty()) {
        found.art.clear();
    }
    session_now = std::move(found);
    session_client = client;
    polled_at = tick();
}

void send_control(const std::string& action) {
    std::string client;
    {
        std::lock_guard lock(mutex);
        client = session_client;
    }
    if (client.empty() || !have_server) return;
    to_server(L"GET", "/player/playback/" + action + "?type=music&commandID=" + std::to_string(tick() & 0xFFFFFF), L"X-Plex-Target-Client-Identifier: " + wide(client) + L"\r\n");
}

void run() noexcept {
    read_kept();
    try {
        std::string saved;
        {
            std::lock_guard lock(mutex);
            saved = auth_token;
        }
        if (!saved.empty()) {
            std::string name;
            bool refused{};
            if (check_token(saved, name, refused)) {
                std::lock_guard lock(mutex);
                if (!name.empty()) user_name = name;
            } else if (refused) {
                {
                    std::lock_guard lock(mutex);
                    auth_token.clear();
                    user_name.clear();
                }
                keep();
                problem("the saved sign-in is no longer valid; sign in again");
            }
        }
    } catch (...) {}
    for (;;) {
        std::string command;
        {
            std::unique_lock lock(mutex);
            is_busy = false;
            wake.wait_for(lock, std::chrono::milliseconds(1500), [] { return !commands.empty(); });
            if (!commands.empty()) {
                command = std::move(commands.front());
                commands.pop_front();
                is_busy = command == "login";
            }
        }
        try {
            if (command == "login") sign_in();
            else if (command == "logout") {
                {
                    std::lock_guard lock(mutex);
                    auth_token.clear();
                    user_name.clear();
                    problem_text.clear();
                    session_now = {};
                }
                have_server = false;
                art_thumb.clear();
                keep();
            } else if (command.starts_with("ctl ")) {
                send_control(command.substr(4));
                Sleep(300);
                poll();
            } else if (tick() - wanted_at.load() < 15000) {
                poll();
            }
        } catch (...) {}
    }
}
void ask(std::string command) {
    start();
    {
        std::lock_guard lock(mutex);
        std::erase(commands, command);
        commands.push_back(std::move(command));
    }
    wake.notify_one();
}
} // namespace

void start() noexcept {
    if (started.exchange(true)) return;
    try {
        std::thread(run).detach();
    } catch (...) {}
}
void login() { ask("login"); }
void logout() { ask("logout"); }
State state() {
    std::lock_guard lock(mutex);
    return {!auth_token.empty(), is_busy, user_name, problem_text};
}
std::string token() {
    std::lock_guard lock(mutex);
    return auth_token;
}
NowPlaying now_playing() {
    start();
    wanted_at.store(tick());
    std::lock_guard lock(mutex);
    NowPlaying copy = session_now;
    copy.valid = copy.valid && !auth_token.empty() && tick() - polled_at < 10000;
    return copy;
}
bool control(Action action) {
    {
        std::lock_guard lock(mutex);
        if (auth_token.empty() || !session_now.valid || session_client.empty()) return false;
    }
    switch (action) {
    case Action::play: ask("ctl play"); break;
    case Action::pause: ask("ctl pause"); break;
    case Action::next: ask("ctl skipNext"); break;
    case Action::previous: ask("ctl skipPrevious"); break;
    }
    return true;
}
std::string client_identifier() {
    std::lock_guard lock(mutex);
    return client_id;
}
}
