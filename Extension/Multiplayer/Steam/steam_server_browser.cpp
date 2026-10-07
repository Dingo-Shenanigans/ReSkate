#include "steam_server_browser.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include "Extension/Multiplayer/developer_identity.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Text/word_filter.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <charconv>
#include <cstring>

namespace dingosdk::multiplayer {
namespace {
constexpr std::uint32_t skate_app = 3354750;
template <class T> T symbol(HMODULE module, const char *name) {
#pragma warning(push)
#pragma warning(disable : 4191)
    return reinterpret_cast<T>(GetProcAddress(module, name));
#pragma warning(pop)
}
// matchmakingtypes.h (CSteamID is pack(1) there, so the ID is unaligned).
#pragma pack(push, 1)
struct PackedSteamId {
    std::uint64_t value;
};
#pragma pack(pop)
struct ServerItem {
    std::uint16_t connection_port, query_port;
    std::uint32_t ip;
    int ping;
    bool had_successful_response, do_not_refresh;
    char game_dir[32], map[32], description[64];
    std::uint32_t app_id;
    int players, max_players, bots;
    bool password, secure;
    std::uint32_t time_last_played;
    int server_version;
    char name[64], tags[128];
    PackedSteamId steam_id;
};
static_assert(sizeof(ServerItem) == 372);
struct Filter {
    char key[256], value[256];
};
// ISteamMatchmakingServerListResponse. Steam calls it from the game's own
// callback pump, on whichever thread that runs; rows are read by polling
// GetServerDetails instead, so it only records completion.
class Response {
  public:
    virtual void ServerResponded(void *, int) {}
    virtual void ServerFailedToRespond(void *, int) {}
    virtual void RefreshComplete(void *, int) { done = true; }
    std::atomic<bool> done{};
};
struct Api {
    HMODULE module{};
    void *(*servers)(){};
    void *(*request)(void *, std::uint32_t, Filter **, std::uint32_t, Response *){};
    void *(*lan)(void *, std::uint32_t, Response *){};
    ServerItem *(*details)(void *, void *, int){};
    int (*count)(void *, void *){};
    bool (*refreshing)(void *, void *){};
    void (*release)(void *, void *){};
    bool open() {
        if (module) return servers != nullptr;
        module = GetModuleHandleW(L"steam_api64.dll");
        if (!module) return false;
        servers = symbol<decltype(servers)>(module, "SteamAPI_SteamMatchmakingServers_v002");
        request = symbol<decltype(request)>(module, "SteamAPI_ISteamMatchmakingServers_RequestInternetServerList");
        lan = symbol<decltype(lan)>(module, "SteamAPI_ISteamMatchmakingServers_RequestLANServerList");
        details = symbol<decltype(details)>(module, "SteamAPI_ISteamMatchmakingServers_GetServerDetails");
        count = symbol<decltype(count)>(module, "SteamAPI_ISteamMatchmakingServers_GetServerCount");
        refreshing = symbol<decltype(refreshing)>(module, "SteamAPI_ISteamMatchmakingServers_IsRefreshing");
        release = symbol<decltype(release)>(module, "SteamAPI_ISteamMatchmakingServers_ReleaseRequest");
        if (!request || !details || !count || !refreshing || !release) servers = nullptr;
        return servers != nullptr;
    }
};
Api &api() {
    static Api value;
    return value;
}
std::string bounded(const char *text, std::size_t size) {
    std::size_t length{};
    while (length < size && text[length]) ++length;
    return std::string(text, length);
}
// A listing's text as it is shown: whole UTF-8 characters, no control characters, at most
// `limit` bytes.
std::string tidy(std::string_view text, std::size_t limit) {
    auto result = clean_chat_text(text);
    if (result.size() > limit) {
        auto cut = limit;
        while (cut && (static_cast<unsigned char>(result[cut]) & 0xC0) == 0x80) --cut;
        result.resize(cut);
    }
    return result;
}
} // namespace

std::optional<MultiplayerLobby> read_server_tags(std::string_view tags, std::uint64_t steam_id) {
    if (!game_server_steam_id(steam_id)) return {};
    MultiplayerLobby row;
    row.id = row.owner = steam_id;
    row.dedicated = true;
    bool reskate{}, version{};
    std::uint64_t secret{};
    const auto integer = [](std::string_view text, int &out) {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    };
    while (!tags.empty()) {
        const auto comma = tags.find(',');
        const auto tag = tags.substr(0, comma);
        tags = comma == std::string_view::npos ? std::string_view{} : tags.substr(comma + 1);
        if (tag == "reskate") { reskate = true; continue; }
        if (tag.empty()) continue;
        const auto value = tag.substr(1);
        switch (tag[0]) {
        case 'v': version = value == std::to_string(protocol_version); break;
        case 'k': {
            const auto result = std::from_chars(value.data(), value.data() + value.size(), secret, 16);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) secret = 0;
            break;
        }
        case 'p': if (!integer(value, row.players)) row.players = 0; break;
        case 'c': if (!integer(value, row.capacity)) row.capacity = 0; break;
        case 'w': row.password_required = value == "1"; break;
        case 'm': row.map = std::string(value); break;
        // The name is last and may itself contain anything but commas.
        case 'n': row.name = std::string(value) + (tags.empty() ? "" : "," + std::string(tags)); tags = {}; break;
        default: break;
        }
    }
    // Anyone can list a server: a row's numbers are held to what a server can have, and its
    // text is cleaned (tidy) before it is shown or logged.
    if (!reskate || !version || !secret || row.capacity < 1 || row.capacity > static_cast<int>(max_remote_players)) return {};
    row.players = std::clamp(row.players, 0, row.capacity);
    row.code = format_invite({steam_id, secret});
    row.name = tidy(row.name, 64);
    row.map = tidy(row.map, 128);
    if (row.name.empty()) row.name = "ReSkate server";
    return row;
}

SteamServerBrowser::~SteamServerBrowser() { release(); }
void SteamServerBrowser::release() {
    for (auto &search : searches_) {
        if (search.request && api().release) api().release(servers_, search.request);
        delete static_cast<Response *>(search.response);
    }
    searches_.clear();
}
const MultiplayerLobby *SteamServerBrowser::find(std::uint64_t id) const {
    for (const auto &row : rows_)
        if (row.id == id) return &row;
    return nullptr;
}
void SteamServerBrowser::refresh(std::uint64_t now) {
    if (!api().open()) return;
    // Steam stops answering a game that asks often (an empty list, for minutes): one search at a
    // time, a quarter of a minute apart, and three quarters after one that came back empty.
    // Opening or refreshing the browser sooner shows what the last search found.
    if (started_ && (!searches_.empty() || now - started_ < (internet_listed_ ? 15000000 : 45000000))) return;
    servers_ = api().servers();
    if (!servers_) return;
    release();
    // What the last search found stays until this one has an internet list of its own; what
    // it says about a server replaces the old copy.
    ++search_;
    internet_listed_ = false;
    for (auto &[id, entry] : found_) entry.answered = false;
    // The internet list, filtered by Steam on the tags; and the LAN list, whose
    // servers answer directly (live map and ping) and show up at once.
    static Filter filter{"gametagsand", {}};
    const auto wanted = "reskate,v" + std::to_string(protocol_version);
    std::memcpy(filter.value, wanted.c_str(), wanted.size() + 1);
    Filter *filters[] = {&filter};
    for (const bool lan : {false, true}) {
        auto *response = new Response;
        void *request = lan ? (api().lan ? api().lan(servers_, skate_app, response) : nullptr)
                            : api().request(servers_, skate_app, filters, 1, response);
        if (request) searches_.push_back({request, response, !lan});
        else delete response;
    }
    started_ = now;
    next_poll_ = now;
}
void SteamServerBrowser::read(std::uint64_t now) {
    for (const auto &search : searches_) {
        const int count = api().count(servers_, search.request);
        if (search.internet && count > 0) internet_listed_ = true;
        for (int i = 0; i < count && i < 512; ++i) {
            const auto *item = api().details(servers_, search.request, i);
            if (!item) continue;
            auto row = read_server_tags(bounded(item->tags, sizeof item->tags), item->steam_id.value);
            if (!row) continue;
            auto &entry = found_[row->id];
            entry.search = search_;
            entry.listed = now;
            if (!search.internet) entry.lan = true;
            // A direct answer is live; the tags come from Steam's master list, which
            // can lag behind a change of map.
            const bool answered = item->had_successful_response;
            if (answered) {
                if (const auto name = tidy(bounded(item->name, sizeof item->name), 64); !name.empty()) row->name = name;
                if (const auto map = tidy(bounded(item->map, sizeof item->map), 128); !map.empty()) row->map = map;
                row->ping = item->ping;
            }
            if (answered || !entry.answered) {
                entry.row = std::move(*row);
                entry.answered = answered;
            }
            const std::pair address{item->ip, item->connection_port};
            if (item->ip && std::find(entry.addresses.begin(), entry.addresses.end(), address) == entry.addresses.end())
                entry.addresses.push_back(address);
        }
    }
    // A restarted anonymous server gets a new, higher Steam ID, while Steam lists the old
    // one for a while. Only one can hold an address and port: keep the newest, or the one
    // signed in with a login token (its ID never changes, and is lower than any anonymous one).
    // Servers named with bad words, or with characters a server name cannot have, are
    // never shown (a server refuses such a name too, but anyone can list one).
    std::vector<MultiplayerLobby> rows;
    for (const auto &[id, entry] : found_) {
        if (!valid_server_name(entry.row.name) || text::contains_bad_words(entry.row.name)) continue;
        if (blocked_server(id) || (server_tokens_required() && !persistent_server_steam_id(id) && !entry.lan)) continue;
        const bool replaced = std::any_of(found_.begin(), found_.end(), [&](const auto &other) {
            const bool kept = persistent_server_steam_id(id), other_kept = persistent_server_steam_id(other.first);
            return (kept != other_kept ? other_kept : other.first > id) && std::any_of(entry.addresses.begin(), entry.addresses.end(), [&](const auto &a) {
                return std::find(other.second.addresses.begin(), other.second.addresses.end(), a) != other.second.addresses.end();
            });
        });
        if (!replaced) rows.push_back(entry.row);
    }
    rows_ = std::move(rows);
}
void SteamServerBrowser::tick(std::uint64_t now) {
    if (searches_.empty() || now < next_poll_) return;
    next_poll_ = now + 250000;
    read(now);
    // Pings of servers behind a NAT never answer; stop waiting after 15 s.
    // Steam can hold an internet search back for seconds before it starts (it does when another
    // was made shortly before), and it is not "refreshing" while it waits: an internet search
    // with nothing listed yet is given six seconds before that counts as finished.
    const bool done = std::all_of(searches_.begin(), searches_.end(), [&](const auto &search) {
        if (static_cast<Response *>(search.response)->done) return true;
        const std::uint64_t patience = search.internet && !internet_listed_ ? 6000000 : 1000000;
        return now - started_ > patience && !api().refreshing(servers_, search.request);
    }) || now - started_ > 15000000;
    if (done) {
        release();
        // An internet list with servers in it is the whole truth: what it left out is gone.
        // An empty one is Steam not answering (see the header), and it has stayed that way for
        // minutes for a player who searched often: keep what was listed in the last quarter
        // of an hour. A server that closed meanwhile only fails to join.
        const auto before = found_.size();
        std::erase_if(found_, [&](const auto &entry) {
            return entry.second.search != search_ && (internet_listed_ || now - entry.second.listed > 900000000);
        });
        if (found_.size() != before) read(now);
        std::string list;
        for (const auto &row : rows_)
            list += (list.empty() ? ": " : "; ") + row.name + (official_server(row.id) ? " [official]" : "") + " (" + row.map + ", " + std::to_string(row.players) + "/" +
                    std::to_string(row.capacity) + (row.ping >= 0 ? ", " + std::to_string(row.ping) + " ms" : std::string{}) + ")";
        logging::log(logging::Level::info, logging::Channel::runtime, "Server browser: {} ReSkate server{} found{}.",
                     rows_.size(), rows_.size() == 1 ? "" : "s", list);
    }
}
} // namespace dingosdk::multiplayer
