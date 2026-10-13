#include "spotify_web.h"
#include "spotify_cover.h"
#include "spotify_device.h"
#include "local_music_ui.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/https_download.h"
#include "Extension/Boot/user_data_redirect.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <objbase.h>
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

#pragma comment(lib, "winhttp")
#pragma comment(lib, "bcrypt")
#pragma comment(lib, "crypt32")
#pragma comment(lib, "ws2_32")
#pragma comment(lib, "shell32")

namespace dingosdk::spotify_web {
namespace {
constexpr wchar_t agent[] = L"ReSkate";
constexpr char scopes[] = "playlist-read-private playlist-read-collaborative user-modify-playback-state user-read-playback-state user-library-read";

std::mutex mutex; // everything below it
std::condition_variable wake;
std::deque<std::string> commands; // "login", "logout", "refresh", "play <id>"
std::string client_id, refresh_token, user_name, problem_text;
std::vector<Playlist> known;
std::vector<Queued> lined_up;
std::atomic<std::uint64_t> queue_asked{};
bool is_busy{};
std::atomic<bool> started{};
// (The worker's alone.)
std::string access_token;
std::uint64_t access_until{};

void say(logging::Level level, const std::string& text) noexcept {
    try {
        logging::log(level, logging::Channel::music, "Spotify account: {}", text);
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
std::string base64url(const unsigned char* data, std::size_t size) {
    static constexpr char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t value = (std::uint32_t{data[i]} << 16) | (i + 1 < size ? std::uint32_t{data[i + 1]} << 8 : 0) | (i + 2 < size ? data[i + 2] : 0);
        out.push_back(digits[value >> 18]);
        out.push_back(digits[(value >> 12) & 63]);
        if (i + 1 < size) out.push_back(digits[(value >> 6) & 63]);
        if (i + 2 < size) out.push_back(digits[value & 63]);
    }
    return out;
}
std::string random_text(std::size_t bytes) {
    std::vector<unsigned char> data(bytes);
    if (BCryptGenRandom(nullptr, data.data(), static_cast<ULONG>(data.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    return base64url(data.data(), data.size());
}
std::string sha256_base64url(const std::string& text) {
    unsigned char digest[32]{};
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())), static_cast<ULONG>(text.size()), digest, sizeof(digest)) != 0) return {};
    return base64url(digest, sizeof(digest));
}

// The token that renews the sign-in, kept so that only this Windows account can read it.
std::string sealed(const std::string& text) {
    DATA_BLOB in{static_cast<DWORD>(text.size()), reinterpret_cast<BYTE*>(const_cast<char*>(text.data()))}, out{};
    if (text.empty() || !CryptProtectData(&in, L"ReSkate Spotify", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
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
std::filesystem::path kept_file() { return redirected_local_app_data().parent_path() / L"spotify.json"; }
void keep() {
    try {
        Json json = Json::object();
        {
            std::lock_guard lock(mutex);
            json["client_id"] = client_id;
            json["refresh_token"] = sealed(refresh_token);
        }
        std::error_code ignored;
        std::filesystem::create_directories(kept_file().parent_path(), ignored);
        std::ofstream(kept_file(), std::ios::binary | std::ios::trunc) << json.dump(2);
    } catch (...) {}
}
void read_kept() {
    try {
        std::ifstream in(kept_file(), std::ios::binary);
        if (!in) return;
        const auto json = Json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
        std::lock_guard lock(mutex);
        client_id = json.value("client_id", "");
        refresh_token = unsealed(json.value("refresh_token", ""));
    } catch (...) {}
}

struct Reply {
    unsigned long status{};
    std::string body;
};
struct Handle {
    HINTERNET h{};
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};
Reply request(const wchar_t* method, const wchar_t* host, const std::wstring& path, const std::wstring& headers, const std::string& body) {
    Reply reply;
    Handle session{WinHttpOpen(agent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h) return reply;
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 15000);
    Handle connection{WinHttpConnect(session.h, host, INTERNET_DEFAULT_HTTPS_PORT, 0)};
    Handle call{connection.h ? WinHttpOpenRequest(connection.h, method, path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr};
    if (!call.h || !WinHttpSendRequest(call.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), static_cast<DWORD>(-1),
            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(call.h, nullptr))
        return reply;
    DWORD status{}, size = sizeof(status);
    WinHttpQueryHeaders(call.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    reply.status = status;
    char buffer[8192];
    for (DWORD got{}; WinHttpReadData(call.h, buffer, sizeof(buffer), &got) && got && reply.body.size() < 4 * 1024 * 1024;) reply.body.append(buffer, got);
    return reply;
}

std::string redirect() { return std::format("http://127.0.0.1:{}/callback", callback_port); }

// The tokens out of Spotify's answer to a sign-in or a renewal.
bool take_tokens(const Reply& reply) {
    try {
        const auto json = Json::parse(reply.body);
        if (reply.status != 200 || !json.contains("access_token")) return false;
        access_token = json.value("access_token", "");
        access_until = GetTickCount64() + (json.contains("expires_in") ? json.at("expires_in").get<std::uint64_t>() : 3600) * 1000;
        if (const auto renewed = json.value("refresh_token", ""); !renewed.empty()) {
            {
                std::lock_guard lock(mutex);
                refresh_token = renewed;
            }
            keep();
        }
        return !access_token.empty();
    } catch (...) {
        return false;
    }
}
// A token to ask with, renewed when it is near its end. Empty when the player is not signed in.
std::string token() {
    if (!access_token.empty() && GetTickCount64() + 60000 < access_until) return access_token;
    std::string client, renewal;
    {
        std::lock_guard lock(mutex);
        client = client_id;
        renewal = refresh_token;
    }
    access_token.clear();
    if (client.empty() || renewal.empty()) return {};
    const auto reply = request(L"POST", L"accounts.spotify.com", L"/api/token", L"Content-Type: application/x-www-form-urlencoded",
        "grant_type=refresh_token&refresh_token=" + escaped(renewal) + "&client_id=" + escaped(client));
    if (!take_tokens(reply)) {
        if (reply.status == 400 || reply.status == 401) { // the sign-in was withdrawn
            {
                std::lock_guard lock(mutex);
                refresh_token.clear();
            }
            keep();
            problem("the sign-in is no longer good; sign in again (music spotify login)");
        } else {
            problem(std::format("Spotify could not be reached to renew the sign-in ({})", reply.status));
        }
        return {};
    }
    return access_token;
}
Reply api(const wchar_t* method, const std::string& path, const std::string& body = {}) {
    const auto bearer = token();
    if (bearer.empty()) return {};
    return request(method, L"api.spotify.com", wide(path), wide("Authorization: Bearer " + bearer) + (body.empty() ? L"" : L"\r\nContent-Type: application/json"), body);
}

// Waits for the browser to come back to the redirect address, and answers it. The code, or
// empty.
std::string await_callback(const std::string& expected_state) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return {};
    std::string code;
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(callback_port);
    if (listener != INVALID_SOCKET && bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 4) == 0) {
        const auto until = GetTickCount64() + 180000;
        while (code.empty() && GetTickCount64() < until) {
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(listener, &ready);
            timeval wait{1, 0};
            if (select(0, &ready, nullptr, nullptr, &wait) <= 0) continue;
            const SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            DWORD timeout = 3000;
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            std::string text;
            char buffer[2048];
            while (text.find("\r\n") == std::string::npos && text.size() < 8192) {
                const int got = recv(client, buffer, sizeof(buffer), 0);
                if (got <= 0) break;
                text.append(buffer, static_cast<std::size_t>(got));
            }
            const auto line = text.substr(0, text.find("\r\n"));
            const auto value = [&](const std::string& name) -> std::string {
                const auto at = line.find(name + "=");
                if (at == std::string::npos) return {};
                const auto from = at + name.size() + 1;
                return line.substr(from, line.find_first_of("& ", from) - from);
            };
            bool good{};
            if (line.starts_with("GET /callback") && value("state") == expected_state) {
                code = value("code");
                good = !code.empty();
                if (!good) code = "-"; // refused in the browser: stop waiting
            }
            const std::string page = std::string("<html><body style=\"font-family:sans-serif;background:#121212;color:#fff;text-align:center;padding-top:20vh\"><h2>") +
                (good ? "ReSkate is signed in to Spotify" : "ReSkate was not signed in") + "</h2><p>You can close this and go back to the game.</p></body></html>";
            const auto answer = std::format("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", page.size(), page);
            send(client, answer.data(), static_cast<int>(answer.size()), 0);
            shutdown(client, SD_SEND);
            closesocket(client);
        }
    } else {
        problem(std::format("port {} on this computer, which the sign-in comes back to, is in use", callback_port));
    }
    if (listener != INVALID_SOCKET) closesocket(listener);
    WSACleanup();
    return code == "-" ? std::string() : code;
}

// The tracks out of Spotify's answer for a playlist's (or the liked songs'): those the music
// screen can show.
std::vector<Track> tracks_of(const Reply& reply) {
    std::vector<Track> tracks;
    if (reply.status != 200) return tracks;
    try {
        const auto json = Json::parse(reply.body);
        if (!json.contains("items") || !json.at("items").is_array()) return tracks;
        const auto& items = json.at("items");
        const auto plain = [](const std::string& text) {
            return !text.empty() && text.size() <= 200 && std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 0x20; });
        };
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& item = items.at(i);
            if (!item.is_object() || !item.contains("track") || !item.at("track").is_object()) continue;
            const auto& song = item.at("track");
            Track track{{}, song.value("name", ""), song.value("uri", "")};
            if (song.contains("artists") && song.at("artists").is_array() && song.at("artists").size() && song.at("artists").at(std::size_t{0}).is_object())
                track.artist = song.at("artists").at(std::size_t{0}).value("name", "");
            if (plain(track.artist) && plain(track.title) && track.uri.starts_with("spotify:track:")) tracks.push_back(std::move(track));
        }
    } catch (...) {}
    return tracks;
}

// What Spotify has lined up (its "queue"): the song playing and those to come.
void fetch_queue() {
    const auto reply = api(L"GET", "/v1/me/player/queue");
    std::vector<Queued> found;
    if (reply.status == 200) {
        try {
            const auto json = Json::parse(reply.body);
            const auto take = [&](const Json& song) {
                if (!song.is_object() || found.size() >= 40) return;
                Queued queued{{}, song.value("name", ""), song.value("uri", ""), {}};
                if (song.contains("artists") && song.at("artists").is_array() && song.at("artists").size() && song.at("artists").at(std::size_t{0}).is_object())
                    queued.artist = song.at("artists").at(std::size_t{0}).value("name", "");
                // (The smallest of its album's pictures: they are listed from the largest.)
                if (song.contains("album") && song.at("album").is_object() && song.at("album").contains("images") && song.at("album").at("images").is_array() && song.at("album").at("images").size()) {
                    const auto& images = song.at("album").at("images");
                    if (const auto& image = images.at(images.size() - 1); image.is_object()) queued.cover = image.value("url", "");
                    if (!queued.cover.starts_with("https://")) queued.cover.clear();
                }
                if (!queued.title.empty()) found.push_back(std::move(queued));
            };
            if (json.contains("currently_playing")) take(json.at("currently_playing"));
            if (json.contains("queue") && json.at("queue").is_array())
                for (std::size_t i = 0; i < json.at("queue").size(); ++i) take(json.at("queue").at(i));
        } catch (...) {}
    }
    std::lock_guard lock(mutex);
    lined_up = std::move(found);
}

void fetch_playlists() {
    std::vector<Playlist> found;
    if (const auto me = api(L"GET", "/v1/me"); me.status == 200) {
        try {
            const auto json = Json::parse(me.body);
            std::lock_guard lock(mutex);
            user_name = json.value("display_name", "");
        } catch (...) {}
    }
    // Liked Songs first, when the sign-in lets them be read (one made before this asked for
    // that does not: signing in again does).
    if (const auto liked = api(L"GET", "/v1/me/tracks?limit=50"); liked.status == 200) found.push_back({liked_songs, "Liked Songs", {}, tracks_of(liked)});
    else if (liked.status == 401 || liked.status == 403) say(logging::Level::info, "Liked Songs need the sign-in made again (music spotify login).");
    const auto reply = api(L"GET", "/v1/me/playlists?limit=50");
    if (reply.status != 200) {
        if (reply.status) problem(std::format("the playlists could not be fetched ({})", reply.status));
        return;
    }
    try {
        const auto json = Json::parse(reply.body);
        if (!json.contains("items") || !json.at("items").is_array()) return;
        const auto& items = json.at("items");
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& item = items.at(i);
            if (!item.is_object()) continue;
            Playlist playlist{item.value("id", ""), item.value("name", ""), {}, {}};
            if (playlist.id.empty() || playlist.name.empty() || playlist.id.size() > 64 ||
                !std::all_of(playlist.id.begin(), playlist.id.end(), [](unsigned char c) { return std::isalnum(c) != 0; }))
                continue;
            // Its picture, fetched and made into what the music screen shows.
            if (item.contains("images") && item.at("images").is_array() && item.at("images").size() && item.at("images").at(std::size_t{0}).is_object()) {
                const auto address = item.at("images").at(std::size_t{0}).value("url", "");
                if (address.starts_with("https://"))
                    if (const auto image = https::get_text(wide(address), 4 * 1024 * 1024, 15, agent))
                        playlist.artwork = spotify_cover::serve(std::vector<std::uint8_t>(image->begin(), image->end()), "playlist-" + playlist.id + ".png");
            }
            playlist.tracks = tracks_of(api(L"GET", "/v1/playlists/" + playlist.id + "/tracks?limit=50"));
            found.push_back(std::move(playlist));
        }
    } catch (...) {
        problem("Spotify's list of playlists could not be read");
        return;
    }
    say(logging::Level::info, std::format("{} playlist(s) fetched.", found.size()));
    {
        std::lock_guard lock(mutex);
        known = std::move(found);
        problem_text.clear();
    }
    profile_runtime::request_music_catalog_refresh();
}

void sign_in() {
    std::string client;
    {
        std::lock_guard lock(mutex);
        client = client_id;
    }
    if (client.empty()) return problem("no client id is set (music spotify client <id>)");
    const auto verifier = random_text(64), state = random_text(16);
    const auto challenge = sha256_base64url(verifier);
    if (verifier.empty() || challenge.empty()) return problem("the sign-in could not be prepared");
    const auto address = "https://accounts.spotify.com/authorize?response_type=code&client_id=" + escaped(client) + "&scope=" + escaped(scopes) +
        "&redirect_uri=" + escaped(redirect()) + "&state=" + state + "&code_challenge_method=S256&code_challenge=" + challenge;
    ShellExecuteW(nullptr, L"open", wide(address).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    say(logging::Level::info, "waiting for the sign-in in the browser.");
    const auto code = await_callback(state);
    if (code.empty()) return problem("the sign-in was not finished in the browser");
    const auto reply = request(L"POST", L"accounts.spotify.com", L"/api/token", L"Content-Type: application/x-www-form-urlencoded",
        "grant_type=authorization_code&code=" + escaped(code) + "&redirect_uri=" + escaped(redirect()) + "&client_id=" + escaped(client) + "&code_verifier=" + verifier);
    if (!take_tokens(reply)) return problem(std::format("Spotify refused the sign-in ({})", reply.status));
    say(logging::Level::info, "signed in.");
    fetch_playlists();
}

void start_playlist(const std::string& playlist, const std::string& track) {
    Json wanted = Json::object();
    if (playlist == liked_songs) {
        // Spotify has nothing to start Liked Songs by: those fetched are handed to it.
        Json songs = Json::array();
        std::uint64_t position{}, at{};
        {
            std::lock_guard lock(mutex);
            for (const auto& known_playlist : known)
                if (known_playlist.id == liked_songs)
                    for (const auto& song : known_playlist.tracks) {
                        if (song.uri == track) position = at;
                        songs.push_back(song.uri);
                        ++at;
                    }
        }
        if (songs.empty()) return problem("there are no Liked Songs to play");
        wanted["uris"] = std::move(songs);
        if (position) {
            Json offset = Json::object();
            offset["position"] = position;
            wanted["offset"] = std::move(offset);
        }
    } else {
        wanted["context_uri"] = "spotify:playlist:" + playlist;
        if (!track.empty()) {
            Json offset = Json::object();
            offset["uri"] = track;
            wanted["offset"] = std::move(offset);
        }
    }
    const auto body = wanted.dump();
    auto reply = api(L"PUT", "/v1/me/player/play", body);
    if (reply.status == 404) { // no player of theirs is the active one: this computer's is made it
        std::string device;
        if (const auto devices = api(L"GET", "/v1/me/player/devices"); devices.status == 200) {
            try {
                const auto json = Json::parse(devices.body);
                const auto& list = json.at("devices");
                for (std::size_t i = 0; i < list.size(); ++i)
                    if (device.empty() || list.at(i).value("type", "") == "Computer") device = list.at(i).value("id", "");
            } catch (...) {}
        }
        if (device.empty()) return problem("Spotify is not open on any device to play the playlist on");
        reply = api(L"PUT", "/v1/me/player/play?device_id=" + escaped(device), body);
    }
    if (reply.status == 403) return problem("Spotify only lets Premium accounts start a playlist from another application");
    if (reply.status != 200 && reply.status != 202 && reply.status != 204) return problem(std::format("the playlist could not be started ({})", reply.status));
    say(logging::Level::debug, "started playlist " + playlist + (track.empty() ? std::string() : " at " + track) + ".");
}

void run() noexcept {
    const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); // (the pictures are converted on this thread)
    read_kept();
    try {
        if (!token().empty()) fetch_playlists();
    } catch (...) {}
    for (;;) {
        std::string command;
        {
            std::unique_lock lock(mutex);
            is_busy = false;
            wake.wait(lock, [] { return !commands.empty(); });
            command = std::move(commands.front());
            commands.pop_front();
            is_busy = command != "queue"; // (a look at the queue is not something the player waits on)
        }
        try {
            if (command == "login") sign_in();
            else if (command == "queue") fetch_queue();
            else if (command == "refresh") fetch_playlists();
            else if (command.starts_with("client ")) {
                {
                    std::lock_guard lock(mutex);
                    client_id = command.substr(7);
                }
                keep();
            }
            else if (command == "logout") {
                access_token.clear();
                {
                    std::lock_guard lock(mutex);
                    refresh_token.clear();
                    known.clear();
                    lined_up.clear();
                    user_name.clear();
                    problem_text.clear();
                }
                keep();
            } else if (command.starts_with("play ")) {
                const auto space = command.find(' ', 5);
                start_playlist(command.substr(5, space == std::string::npos ? space : space - 5), space == std::string::npos ? std::string() : command.substr(space + 1));
            }
        } catch (...) {}
    }
    if (initialized) CoUninitialize();
}
void ask(std::string command) {
    start();
    {
        std::lock_guard lock(mutex);
        // (Only the last playlist asked for matters.)
        if (command.starts_with("play ")) std::erase_if(commands, [](const std::string& waiting) { return waiting.starts_with("play "); });
        if (command == "queue") std::erase_if(commands, [](const std::string& waiting) { return waiting == "queue"; });
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
void set_client(const std::string& id) { ask("client " + id); }
std::string client() {
    std::lock_guard lock(mutex);
    return client_id;
}
void login() { ask("login"); }
void logout() { ask("logout"); }
void refresh() { ask("refresh"); }
void play(const std::string& playlist, const std::string& track) {
    // On the game's own device (spotify_device.h) when an account is on it: it is told itself.
    if (spotify_device::connected()) {
        if (playlist != liked_songs) return spotify_device::load("spotify:playlist:" + playlist, track);
        std::string tracks;
        unsigned index{}, at{};
        {
            std::lock_guard lock(mutex);
            for (const auto& known_playlist : known)
                if (known_playlist.id == liked_songs)
                    for (const auto& song : known_playlist.tracks) {
                        if (song.uri == track) index = at;
                        tracks += (tracks.empty() ? "" : " ") + song.uri;
                        ++at;
                    }
        }
        if (!tracks.empty()) spotify_device::load_tracks(tracks, index);
        return;
    }
    ask("play " + playlist + (track.empty() ? std::string() : " " + track));
}
std::vector<Playlist> playlists() {
    std::lock_guard lock(mutex);
    return known;
}
std::vector<Queued> queue() {
    std::lock_guard lock(mutex);
    return refresh_token.empty() ? std::vector<Queued>{} : lined_up;
}
void want_queue(bool now) {
    {
        std::lock_guard lock(mutex);
        if (refresh_token.empty()) return;
    }
    const auto tick = GetTickCount64();
    if (!now && tick - queue_asked.load(std::memory_order_relaxed) < 4000) return;
    queue_asked.store(tick, std::memory_order_relaxed);
    ask("queue");
}
State state() {
    std::lock_guard lock(mutex);
    return {!client_id.empty(), !refresh_token.empty(), is_busy, user_name, problem_text, known.size()};
}
} // namespace dingosdk::spotify_web
