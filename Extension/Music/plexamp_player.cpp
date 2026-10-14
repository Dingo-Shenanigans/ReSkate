#include "plexamp_player.h"
#include "plex_login.h"
#include <Windows.h>
#include <winhttp.h>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::plexamp_player {
namespace {
constexpr wchar_t host[] = L"127.0.0.1";
constexpr INTERNET_PORT port = 32500;
constexpr std::size_t largest_reply = 256 * 1024;
std::atomic<unsigned> command_id{1};

struct Handle {
    HINTERNET h{};
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

bool get(const std::wstring& path, std::string* reply) noexcept {
    try {
        Handle session(WinHttpOpen(L"ReSkate", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session.h) return false;
        WinHttpSetTimeouts(session.h, 1000, 1000, 1500, 2000);
        Handle connection(WinHttpConnect(session.h, host, port, 0));
        if (!connection.h) return false;
        Handle request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
        if (!request.h) return false;
        constexpr wchar_t headers[] = L"X-Plex-Client-Identifier: reskate-music\r\nX-Plex-Device-Name: ReSkate\r\nX-Plex-Product: ReSkate\r\n";
        if (!WinHttpSendRequest(request.h, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.h, nullptr)) return false;
        DWORD status{}, size = sizeof(status);
        if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) return false;
        if (!reply) return true;
        for (;;) {
            DWORD available{};
            if (!WinHttpQueryDataAvailable(request.h, &available) || !available) break;
            const auto at = reply->size();
            if (at + available > largest_reply) return false;
            reply->resize(at + available);
            DWORD read{};
            if (!WinHttpReadData(request.h, reply->data() + at, available, &read)) return false;
            reply->resize(at + read);
        }
        return true;
    } catch (...) {
        return false;
    }
}

// The start of the first element of this name, or npos.
std::size_t element(const std::string& xml, std::string_view name, std::size_t from = 0) {
    const std::string open = "<" + std::string(name);
    for (auto at = xml.find(open, from); at != std::string::npos; at = xml.find(open, at + 1)) {
        const auto after = at + open.size();
        if (after < xml.size() && (xml[after] == ' ' || xml[after] == '/' || xml[after] == '>')) return at;
    }
    return std::string::npos;
}
std::string tag_of(const std::string& xml, std::size_t start) {
    const auto end = xml.find('>', start);
    return end == std::string::npos ? std::string() : xml.substr(start, end - start);
}
void append_utf8(std::string& out, unsigned code) {
    if (code < 0x80) out += static_cast<char>(code);
    else if (code < 0x800) out += {static_cast<char>(0xC0 | (code >> 6)), static_cast<char>(0x80 | (code & 0x3F))};
    else if (code < 0x10000) out += {static_cast<char>(0xE0 | (code >> 12)), static_cast<char>(0x80 | ((code >> 6) & 0x3F)), static_cast<char>(0x80 | (code & 0x3F))};
    else out += {static_cast<char>(0xF0 | (code >> 18)), static_cast<char>(0x80 | ((code >> 12) & 0x3F)), static_cast<char>(0x80 | ((code >> 6) & 0x3F)), static_cast<char>(0x80 | (code & 0x3F))};
}
std::string unescape(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        const auto end = text.find(';', i);
        if (end == std::string_view::npos || end - i > 10) {
            out += text[i];
            continue;
        }
        const auto entity = text.substr(i + 1, end - i - 1);
        if (entity == "amp") out += '&';
        else if (entity == "lt") out += '<';
        else if (entity == "gt") out += '>';
        else if (entity == "quot") out += '"';
        else if (entity == "apos") out += '\'';
        else if (entity.size() > 1 && entity[0] == '#') {
            try {
                const bool hex = entity[1] == 'x' || entity[1] == 'X';
                append_utf8(out, static_cast<unsigned>(std::stoul(std::string(entity.substr(hex ? 2 : 1)), nullptr, hex ? 16 : 10)));
            } catch (...) {}
        } else {
            out += text.substr(i, end - i + 1);
        }
        i = end;
    }
    return out;
}
// An attribute of an element's tag ("" when it has none). The name is matched after a space,
// so "title" is not "grandparentTitle".
std::string attribute(const std::string& tag, std::string_view name) {
    const std::string key = " " + std::string(name) + "=\"";
    const auto at = tag.find(key);
    if (at == std::string::npos) return {};
    const auto begin = at + key.size();
    const auto end = tag.find('"', begin);
    return end == std::string::npos ? std::string() : unescape(std::string_view(tag).substr(begin, end - begin));
}
} // namespace

namespace {
std::string escape(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else out += std::format("%{:02X}", c);
    }
    return out;
}
} // namespace

external_player::Track look() {
    external_player::Track track;
    std::string xml;
    if (!get(L"/player/timeline/poll?wait=0&includeMetadata=1&commandID=" + std::to_wstring(command_id.fetch_add(1)), &xml)) return track;
    // (One timeline for each kind of media: the music one is the one that matters.)
    std::string timeline;
    for (auto at = element(xml, "Timeline"); at != std::string::npos; at = element(xml, "Timeline", at + 1))
        if (auto tag = tag_of(xml, at); attribute(tag, "type") == "music") {
            timeline = std::move(tag);
            break;
        }
    const auto state = attribute(timeline, "state");
    if (state != "playing" && state != "paused" && state != "buffering") return track;
    const auto at = element(xml, "Track");
    if (at == std::string::npos) return track;
    const auto song = tag_of(xml, at);
    track.available = true;
    track.app = "Plexamp";
    track.playing = state != "paused";
    track.title = attribute(song, "title");
    track.artist = attribute(song, "originalTitle");
    if (track.artist.empty()) track.artist = attribute(song, "grandparentTitle");
    track.album = attribute(song, "parentTitle");
    const auto controllable = "," + attribute(timeline, "controllable") + ",";
    track.can_next = controllable.find(",skipNext,") != std::string::npos;
    track.can_previous = controllable.find(",skipPrevious,") != std::string::npos;
    track.can_pause = track.can_play = controllable.find(",playPause,") != std::string::npos;
    // The cover: from the Plex server the song is on, which wants the player's token. Asked
    // for once for a song (and again once the player has signed in).
    auto thumb = attribute(song, "parentThumb");
    if (thumb.empty()) thumb = attribute(song, "thumb");
    static std::string art_of, art_token;
    static std::vector<std::uint8_t> art;
    static std::uint64_t art_revision{};
    if (const auto token = plex_login::token(); !thumb.empty() && !token.empty() && (thumb != art_of || token != art_token)) {
        art_of = thumb;
        art_token = token;
        std::vector<std::uint8_t> bytes;
        const auto server = attribute(timeline, "address");
        const auto reached = static_cast<unsigned short>(std::strtoul(attribute(timeline, "port").c_str(), nullptr, 10));
        const bool secure = attribute(timeline, "protocol") == "https";
        if (!server.empty() && reached) {
            // (Sized down by the server when it will; as it is when it will not.)
            for (const auto& path : {"/photo/:/transcode?width=512&height=512&minSize=1&url=" + escape(thumb) + "&X-Plex-Token=" + token, thumb + "?X-Plex-Token=" + token}) {
                const auto reply = plex_login::get(server, reached, secure, path);
                if (reply.status == 200 && !reply.body.empty()) {
                    bytes.assign(reply.body.begin(), reply.body.end());
                    break;
                }
            }
        }
        if (bytes != art) {
            art = std::move(bytes);
            ++art_revision;
        }
    }
    if (thumb.empty() && !art.empty()) {
        art.clear();
        art_of.clear();
        ++art_revision;
    }
    track.artwork = art;
    track.artwork_revision = art_revision;
    return track;
}

bool send(external_player::Command command) noexcept {
    using external_player::Command;
    const wchar_t* action = command == Command::play ? L"play" : command == Command::pause ? L"pause" : command == Command::next ? L"skipNext" : command == Command::previous ? L"skipPrevious" : L"playPause";
    try {
        return get(std::wstring(L"/player/playback/") + action + L"?type=music&commandID=" + std::to_wstring(command_id.fetch_add(1)), nullptr);
    } catch (...) {
        return false;
    }
}
} // namespace dingosdk::plexamp_player
