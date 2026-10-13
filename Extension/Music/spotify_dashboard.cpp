#include "spotify_dashboard.h"
#include "external_player.h"
#include "external_audio.h"
#include "youtube_music.h"
#include "app_audio_capture.h"
#include "audio_dsp.h"
#include "local_music_ui.h"
#include "music_inject.h"
#include "spotify_cover.h"
#include "spotify_device.h"
#include "spotify_station.h"
#include "spotify_web.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Boot/user_data_redirect.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>

namespace dingosdk::spotify_dashboard {
namespace {
constexpr unsigned short first_port = 8421;
std::atomic<unsigned short> port{};
std::atomic<bool> started{};

// Something built into the DLL (cmake/Runtime.cmake): the page itself (setup_page.html), and
// the mod's own fonts, which the page is set in.
std::string built_in(const wchar_t* name) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&built_in), &module)) return {};
    const auto resource = FindResourceW(module, name, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    const auto handle = resource ? LoadResource(module, resource) : nullptr;
    const auto* data = handle ? static_cast<const char*>(LockResource(handle)) : nullptr;
    return data ? std::string(data, SizeofResource(module, resource)) : std::string();
}

// The sound's settings, as the player left them: <Local AppData>\\ReSkate\\music-settings.json.
std::filesystem::path settings_file() { return redirected_local_app_data().parent_path() / L"music-settings.json"; }
void keep_settings() {
    try {
        Json json = Json::object();
        json["gain"] = static_cast<std::uint64_t>(music_inject::gain() * 100.0f + 0.5f);
        json["speakers"] = music_inject::mode() != 3;
        json["levelling"] = audio_dsp::levelling();
        json["notices"] = youtube_music::notices();
        Json bands = Json::array();
        for (int i = 0; i < audio_dsp::bands; ++i) bands.push_back(static_cast<std::int64_t>(std::lround(audio_dsp::band(i))));
        json["equaliser"] = std::move(bands);
        std::ofstream(settings_file(), std::ios::binary | std::ios::trunc) << json.dump(2);
    } catch (...) {}
}
void read_settings() {
    try {
        std::ifstream in(settings_file(), std::ios::binary);
        if (!in) return;
        const auto json = Json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
        if (json.contains("gain") && json.at("gain").is_number()) music_inject::set_gain(static_cast<float>(std::clamp<std::int64_t>(json.at("gain").get<std::int64_t>(), 0, 400)) / 100.0f);
        if (json.contains("speakers") && json.at("speakers").is_boolean()) music_inject::set_mode(json.at("speakers").get<bool>() ? 1 : 3);
        if (json.contains("levelling") && json.at("levelling").is_boolean()) audio_dsp::set_levelling(json.at("levelling").get<bool>());
        if (json.contains("notices") && json.at("notices").is_boolean()) youtube_music::set_notices(json.at("notices").get<bool>());
        if (json.contains("equaliser") && json.at("equaliser").is_array())
            for (std::size_t i = 0; i < json.at("equaliser").size() && i < audio_dsp::bands; ++i)
                if (json.at("equaliser").at(i).is_number()) audio_dsp::set_band(static_cast<int>(i), static_cast<float>(std::clamp<std::int64_t>(json.at("equaliser").at(i).get<std::int64_t>(), -12, 12)));
    } catch (...) {}
}

std::string state_json() {
    Json out = Json::object();
    const auto track = external_player::now();
    Json now = Json::object();
    now["artist"] = track.available ? track.artist : std::string();
    now["title"] = track.available ? track.title : std::string();
    now["playing"] = track.available && track.playing;
    now["cover"] = track.available ? spotify_cover::url() : std::string();
    now["loading"] = youtube_music::on() && youtube_music::state().loading;
    out["now"] = std::move(now);
    out["station"] = spotify_station::active();
    Json device = Json::object();
    device["connected"] = spotify_device::connected();
    device["in_use"] = spotify_device::in_use();
    device["text"] = spotify_device::status();
    out["device"] = std::move(device);
    const auto account_state = spotify_web::state();
    Json account = Json::object();
    account["has_client"] = account_state.has_client;
    account["signed_in"] = account_state.signed_in;
    account["busy"] = account_state.busy;
    account["user"] = account_state.user;
    account["problem"] = account_state.problem;
    account["client"] = spotify_web::client();
    Json playlists = Json::array();
    for (const auto& playlist : spotify_web::playlists()) {
        Json entry = Json::object();
        entry["name"] = playlist.name;
        entry["tracks"] = static_cast<std::uint64_t>(playlist.tracks.size());
        playlists.push_back(std::move(entry));
    }
    account["playlists"] = std::move(playlists);
    out["account"] = std::move(account);
    {
        const auto tube = youtube_music::state();
        Json youtube = Json::object();
        youtube["available"] = tube.available;
        youtube["tools"] = tube.tools;
        youtube["busy"] = tube.busy;
        youtube["percent"] = static_cast<std::uint64_t>(tube.percent);
        youtube["problem"] = tube.problem;
        youtube["browser"] = tube.browser;
        youtube["cookies"] = tube.cookies;
        youtube["runtime"] = tube.runtime;
        // The playlists of each kind (youtube_music.h), each with its place among them all,
        // which is what it is taken out by.
        using youtube_music::Kind;
        Json lists = Json::array(), folders = Json::array(), stations = Json::array();
        for (std::size_t i = 0; i < tube.playlists.size(); ++i) {
            const auto& playlist = tube.playlists[i];
            Json entry = Json::object();
            entry["index"] = static_cast<std::uint64_t>(i);
            entry["name"] = playlist.name;
            entry["songs"] = static_cast<std::uint64_t>(playlist.songs.size());
            if (playlist.kind == Kind::files) entry["path"] = playlist.url;
            if (playlist.kind == Kind::radio && !playlist.songs.empty()) entry["address"] = playlist.songs.front().source;
            (playlist.kind == Kind::files ? folders : playlist.kind == Kind::radio ? stations : lists).push_back(std::move(entry));
        }
        youtube["playlists"] = std::move(lists);
        const auto problem_of = [&](Kind kind) { return tube.problem_kind == kind ? tube.problem : std::string(); };
        youtube["problem"] = problem_of(Kind::youtube);
        out["youtube"] = std::move(youtube);
        Json local = Json::object();
        local["available"] = tube.available;
        local["busy"] = tube.busy;
        local["problem"] = problem_of(Kind::files);
        local["folders"] = std::move(folders);
        out["local"] = std::move(local);
        Json radio = Json::object();
        radio["available"] = tube.available;
        radio["busy"] = tube.busy;
        radio["problem"] = problem_of(Kind::radio);
        radio["stations"] = std::move(stations);
        radio["searched"] = tube.searched;
        Json found = Json::array();
        for (const auto& station : tube.found) {
            Json entry = Json::object();
            entry["name"] = station.name;
            entry["address"] = station.address;
            entry["icon"] = station.icon;
            entry["about"] = station.about;
            found.push_back(std::move(entry));
        }
        radio["found"] = std::move(found);
        out["radio"] = std::move(radio);
        // What is lined up: the songs of the playlist that is playing, when it is one of these.
        if (tube.playing && tube.playing_list < tube.playlists.size()) {
            const auto& playlist = tube.playlists[tube.playing_list];
            Json queue = Json::object();
            queue["playlist"] = static_cast<std::uint64_t>(tube.playing_list);
            queue["name"] = playlist.name;
            queue["kind"] = playlist.kind == Kind::files ? "local" : playlist.kind == Kind::radio ? "radio" : "youtube";
            queue["at"] = static_cast<std::uint64_t>(tube.playing_song);
            Json songs = Json::array();
            for (const auto& song : playlist.songs) {
                Json entry = Json::object();
                entry["artist"] = song.artist;
                entry["title"] = song.title;
                // Its picture: its video's and its station's from where they are; its file's from here.
                entry["cover"] = playlist.kind == Kind::youtube ? "https://i.ytimg.com/vi/" + song.id + "/mqdefault.jpg"
                    : playlist.kind == Kind::radio ? (playlist.url.starts_with("https://") ? playlist.url : std::string())
                    : std::format("/api/art/{}/{}?{}", tube.playing_list, songs.size(), song.id);
                songs.push_back(std::move(entry));
            }
            queue["songs"] = std::move(songs);
            out["queue"] = std::move(queue);
        }
    }
    // (And Spotify's own queue, when Spotify is what is playing and the player has signed in:
    // the song playing, then those Spotify says come after it.)
    if (!out.contains("queue") && spotify_station::active() && !youtube_music::on() && !external_audio::on() && account_state.signed_in) {
        spotify_web::want_queue();
        // (Looked at again at once when the song has changed.)
        static std::atomic<std::uint64_t> seen{};
        if (seen.exchange(track.revision) != track.revision) spotify_web::want_queue(true);
        if (const auto lined_up = spotify_web::queue(); !lined_up.empty()) {
            Json queue = Json::object();
            queue["playlist"] = static_cast<std::uint64_t>(0);
            queue["name"] = "Spotify";
            queue["kind"] = "spotify";
            queue["at"] = static_cast<std::uint64_t>(0);
            Json songs = Json::array();
            for (const auto& song : lined_up) {
                Json entry = Json::object();
                entry["artist"] = song.artist;
                entry["title"] = song.title;
                entry["cover"] = song.cover;
                songs.push_back(std::move(entry));
            }
            queue["songs"] = std::move(songs);
            out["queue"] = std::move(queue);
        }
    }
    Json external = Json::object();
    external["app"] = external_audio::app();
    external["on"] = external_audio::on();
    {
        // (How its capture is going, for the player who hears nothing.)
        const auto capture = app_audio::state();
        external["capture"] = !external_audio::on() ? std::string()
            : !capture.capturing ? "Not capturing it" + (capture.problem.empty() ? std::string() : ": " + capture.problem)
            : capture.level > 0.0005f ? std::string("Capturing it: sound is coming through") : std::string("Capturing it, but it is silent: is it playing, and not muted in Windows' volume mixer?");
    }
    Json apps = Json::array();
    for (const auto& name : external_audio::apps()) apps.push_back(name);
    external["apps"] = std::move(apps);
    out["external"] = std::move(external);
    Json settings = Json::object();
    settings["gain"] = static_cast<std::uint64_t>(music_inject::gain() * 100.0f + 0.5f);
    settings["speakers"] = music_inject::mode() != 3;
    settings["levelling"] = audio_dsp::levelling();
    Json bands = Json::array();
    for (int i = 0; i < audio_dsp::bands; ++i) bands.push_back(static_cast<std::int64_t>(std::lround(audio_dsp::band(i))));
    settings["equaliser"] = std::move(bands);
    out["settings"] = std::move(settings);
    return out.dump();
}

// What a change asks for, done. False when it is not one of them.
bool change(const std::string& path, const std::string& body) {
    Json json = Json::object();
    try {
        if (!body.empty()) json = Json::parse(body);
    } catch (...) {
        return false;
    }
    if (!json.is_object()) return false;
    if (path == "/api/command") {
        const auto command = json.value("command", "");
        using external_player::Command;
        if (command == "play") external_player::send(Command::play);
        else if (command == "pause") external_player::send(Command::pause);
        else if (command == "toggle") external_player::send(Command::toggle);
        else if (command == "next") external_player::send(Command::next);
        else if (command == "previous") external_player::send(Command::previous);
        else return false;
    } else if (path == "/api/client") {
        const auto id = json.value("id", "");
        if (id.empty() || id.size() > 64 || !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) != 0; })) return false;
        spotify_web::set_client(id);
    } else if (path == "/api/login") {
        spotify_web::login();
    } else if (path == "/api/logout") {
        spotify_web::logout();
    } else if (path == "/api/refresh") {
        spotify_web::refresh();
    } else if (path == "/api/youtube/tools") {
        youtube_music::fetch_tools();
    } else if (path == "/api/youtube/add") {
        youtube_music::add(json.value("link", ""));
    } else if (path == "/api/youtube/cookies") {
        if (!youtube_music::set_cookies(json.value("text", ""))) return false;
    } else if (path == "/api/youtube/browser") {
        youtube_music::set_browser(json.value("browser", ""));
    } else if (path == "/api/local/add") {
        youtube_music::add_folder(json.value("path", ""));
    } else if (path == "/api/radio/search") {
        youtube_music::search_radio(json.value("query", ""));
    } else if (path == "/api/radio/add") {
        youtube_music::add_station(json.value("name", ""), json.value("address", ""), json.value("icon", ""));
    } else if (path == "/api/queue/play") {
        if (!json.contains("playlist") || !json.at("playlist").is_number() || !json.contains("song") || !json.at("song").is_number()) return false;
        // Spotify has no way to go straight to a song of its queue: it is gone on to, song by song.
        if (json.contains("spotify") && json.at("spotify").is_boolean() && json.at("spotify").get<bool>()) {
            const auto count = static_cast<int>(std::clamp<std::int64_t>(json.at("song").get<std::int64_t>(), 0, 25));
            // (One at a time: a press while it is still going on is not taken.)
            static std::atomic<bool> going{};
            if (going.exchange(true)) return true;
            std::thread([count] {
                for (int i = 0; i < count; ++i) {
                    external_player::send(external_player::Command::next);
                    Sleep(450);
                }
                spotify_web::want_queue(true);
                going.store(false);
            }).detach();
            return true;
        }
        youtube_music::play_at(static_cast<std::size_t>(std::clamp<std::int64_t>(json.at("playlist").get<std::int64_t>(), 0, 1000)),
            static_cast<std::size_t>(std::clamp<std::int64_t>(json.at("song").get<std::int64_t>(), 0, 100000)));
    } else if (path == "/api/youtube/remove" || path == "/api/library/remove") {
        if (!json.contains("index") || !json.at("index").is_number()) return false;
        youtube_music::remove(static_cast<std::size_t>(std::clamp<std::int64_t>(json.at("index").get<std::int64_t>(), 0, 1000)));
    } else if (path == "/api/external") {
        external_audio::set_app(json.value("app", ""));
        profile_runtime::request_music_catalog_refresh();
        if (external_audio::on()) external_player::use_app(external_audio::app());
    } else if (path == "/api/settings") {
        if (json.contains("gain") && json.at("gain").is_number()) music_inject::set_gain(static_cast<float>(std::clamp<std::int64_t>(json.at("gain").get<std::int64_t>(), 0, 400)) / 100.0f);
        if (json.contains("speakers") && json.at("speakers").is_boolean()) music_inject::set_mode(json.at("speakers").get<bool>() ? 1 : 3);
        if (json.contains("levelling") && json.at("levelling").is_boolean()) audio_dsp::set_levelling(json.at("levelling").get<bool>());
        if (json.contains("equaliser") && json.at("equaliser").is_array())
            for (std::size_t i = 0; i < json.at("equaliser").size() && i < audio_dsp::bands; ++i)
                if (json.at("equaliser").at(i).is_number()) audio_dsp::set_band(static_cast<int>(i), static_cast<float>(std::clamp<std::int64_t>(json.at("equaliser").at(i).get<std::int64_t>(), -12, 12)));
        keep_settings();
    } else {
        return false;
    }
    return true;
}

void answer(SOCKET client, const char* status, const char* type, const std::string& body, const char* cache = "no-store") {
    const auto head = std::format("HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nCache-Control: {}\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n", status, type,
        body.size(), cache);
    send(client, head.data(), static_cast<int>(head.size()), 0);
    for (std::size_t sent = 0; sent < body.size();) {
        const int did = send(client, body.data() + sent, static_cast<int>(std::min<std::size_t>(body.size() - sent, 65536)), 0);
        if (did <= 0) break;
        sent += static_cast<std::size_t>(did);
    }
}
std::string header(const std::string& request, const std::string& name) {
    // (Names are matched as browsers write them here.)
    const auto at = request.find("\r\n" + name + ": ");
    if (at == std::string::npos) return {};
    const auto from = at + name.size() + 4;
    return request.substr(from, request.find("\r\n", from) - from);
}

void serve(SOCKET client) {
    DWORD timeout = 3000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::string request;
    char buffer[4096];
    std::size_t head_end = std::string::npos;
    while (head_end == std::string::npos && request.size() < 16384) {
        const int got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) return;
        request.append(buffer, static_cast<std::size_t>(got));
        head_end = request.find("\r\n\r\n");
    }
    if (head_end == std::string::npos) return;
    std::size_t length{};
    try {
        if (const auto text = header(request, "Content-Length"); !text.empty()) length = std::stoul(text);
    } catch (...) {
        return;
    }
    if (length > 2 * 1024 * 1024) return answer(client, "413 Payload Too Large", "text/plain", "too large");
    while (request.size() < head_end + 4 + length) {
        const int got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) return;
        request.append(buffer, static_cast<std::size_t>(got));
    }
    const auto line = request.substr(0, request.find("\r\n"));
    const auto method = line.substr(0, line.find(' '));
    const auto path_from = method.size() + 1;
    const auto path = line.substr(path_from, line.find(' ', path_from) - path_from);
    // Only as itself: a request that names another host (a site made to resolve to this
    // computer) is not answered.
    const auto host = header(request, "Host");
    const auto own = std::to_string(port.load());
    if (host != "127.0.0.1:" + own && host != "localhost:" + own) return answer(client, "403 Forbidden", "text/plain", "not for this address");
    if (method == "GET" && (path == "/" || path == "/index.html")) {
        const auto page = built_in(L"MUSIC_SETUP_HTML");
        return page.empty() ? answer(client, "500 Internal Server Error", "text/plain", "the page was not built into this ReSkate.dll") : answer(client, "200 OK", "text/html; charset=utf-8", page);
    }
    if (method == "GET" && path.starts_with("/font/")) {
        const auto font = built_in(path == "/font/heading.ttf" ? L"FONT_HEADING" : path == "/font/body.ttf" ? L"FONT_BODY" : path == "/font/brush.ttf" ? L"FONT_BRUSH" : L"");
        return font.empty() ? answer(client, "404 Not Found", "text/plain", "no such font") : answer(client, "200 OK", "font/ttf", font, "public, max-age=86400");
    }
    if (method == "GET" && path.starts_with("/api/art/")) {
        // The picture in a file of the player's, for the queue: /api/art/<playlist>/<song>.
        std::size_t playlist{}, song{};
        if (sscanf_s(path.c_str(), "/api/art/%zu/%zu", &playlist, &song) != 2) return answer(client, "404 Not Found", "text/plain", "no such picture");
        const auto image = youtube_music::art(playlist, song);
        const bool png = image.size() > 8 && image[0] == 0x89 && image[1] == 'P', jpeg = image.size() > 8 && image[0] == 0xff && image[1] == 0xd8;
        if (!png && !jpeg) return answer(client, "404 Not Found", "text/plain", "no picture", "private, max-age=3600");
        return answer(client, "200 OK", png ? "image/png" : "image/jpeg", std::string(image.begin(), image.end()), "private, max-age=3600");
    }
    if (method == "GET" && path == "/api/state") return answer(client, "200 OK", "application/json", state_json());
    if (method == "POST") {
        // A change comes from this page alone: its script sends a header that a page of another
        // site cannot send without the browser asking first, which is refused by not answering.
        if (header(request, "X-ReSkate") != "1") return answer(client, "403 Forbidden", "text/plain", "not from the page");
        return change(path, request.substr(head_end + 4, length)) ? answer(client, "200 OK", "application/json", "{}") : answer(client, "400 Bad Request", "text/plain", "not understood");
    }
    answer(client, "404 Not Found", "text/plain", "nothing here");
}

void run() noexcept {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
    SOCKET listener = INVALID_SOCKET;
    for (unsigned short candidate = first_port; candidate < first_port + 10 && listener == INVALID_SOCKET; ++candidate) {
        const SOCKET made = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        at.sin_port = htons(candidate);
        if (made != INVALID_SOCKET && bind(made, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0 && listen(made, 8) == 0) {
            listener = made;
            port.store(candidate);
        } else if (made != INVALID_SOCKET) {
            closesocket(made);
        }
    }
    if (listener == INVALID_SOCKET) {
        try {
            logging::log(logging::Level::warning, logging::Channel::music, "Music setup page: no port on this computer was free for it.");
        } catch (...) {}
        return;
    }
    try {
        logging::log(logging::Level::info, logging::Channel::music, "Music setup page: {} (\"music setup\" in the console opens it).", address());
    } catch (...) {}
    for (;;) {
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            Sleep(50);
            continue;
        }
        try {
            serve(client);
        } catch (...) {}
        shutdown(client, SD_SEND);
        closesocket(client);
    }
}
} // namespace

void start() noexcept {
    if (started.exchange(true)) return;
    read_settings();
    try {
        std::thread(run).detach();
    } catch (...) {}
}
void save_settings() noexcept { keep_settings(); }
std::string address() {
    const auto at = port.load();
    return at ? "http://127.0.0.1:" + std::to_string(at) + "/" : std::string();
}
bool open() noexcept {
    try {
        const auto where = address();
        if (where.empty()) return false;
        const std::wstring wide(where.begin(), where.end());
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    } catch (...) {
        return false;
    }
}
} // namespace dingosdk::spotify_dashboard
