#include "developer_identity.h"
#include "Engine/Core/Json/json.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <memory>
#include <stdexcept>

// The lists themselves, shared by the game and the dedicated server. Each reads
// them from the backend its own way: developer_identity_fetch.cpp, Server/global_bans.cpp.
namespace dingosdk::multiplayer {
namespace {
// The API's name for each category of players. The ban list and the servers come beside them.
constexpr std::array<std::pair<std::string_view, IdentityList>, 5> categories{{
    {"dev", IdentityList::developer}, {"staff", IdentityList::staff}, {"homie", IdentityList::homie},
    {"content_creator", IdentityList::content_creator}, {"centrix", IdentityList::centrix}}};
constexpr auto banned = static_cast<std::size_t>(IdentityList::banned);
constexpr auto servers = static_cast<std::size_t>(IdentityList::official_server);
constexpr auto blocked = static_cast<std::size_t>(IdentityList::blocked_server);

std::atomic<std::shared_ptr<const IdentityLists>> current;

// A player's SteamID64 is 76561197960265728 plus a 32-bit account number, and
// nobody has account 0. The API sends them as strings.
std::optional<std::uint64_t> number(const Json &entry) {
    std::uint64_t id{};
    if (!entry.is_string()) return std::nullopt;
    const auto &text = entry.string();
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? std::optional(id) : std::nullopt;
}
std::uint64_t steam_id(const Json &entry) {
    constexpr std::uint64_t base = 76561197960265728ULL, last = base + 0xffffffffULL;
    if (const auto id = number(entry); id && *id > base && *id <= last) return *id;
    throw std::runtime_error("an entry is not a player's SteamID64");
}
// Only a server signed in with a login token keeps its ID; an anonymous one's is gone at its
// next start, so listing it would mean nothing.
std::uint64_t server_id(const Json &entry) {
    if (const auto id = number(entry); id && persistent_server_steam_id(*id)) return *id;
    throw std::runtime_error("an entry is not the SteamID64 of a server with a login token");
}
// A block can name an anonymous server too: its ID is all there is to hold on to.
std::uint64_t any_server_id(const Json &entry) {
    if (const auto id = number(entry); id && game_server_steam_id(*id)) return *id;
    throw std::runtime_error("an entry is not the SteamID64 of a server");
}
} // namespace

IdentityLists parse_identity_lists(std::string_view json) {
    const auto answer = Json::parse(json);
    if (!answer.is_object() || !answer.contains("categories") || !answer.at("categories").is_object())
        throw std::runtime_error("the answer has no categories");
    const auto &listed = answer.at("categories");
    IdentityLists lists;
    const auto read = [](const Json &entries, std::vector<std::uint64_t> &ids, std::uint64_t (*id)(const Json &) = steam_id) {
        if (!entries.is_array()) throw std::runtime_error("a list of players is not a list");
        for (const auto &entry : entries) ids.push_back(id(entry));
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    };
    for (const auto &[name, list] : categories)
        if (listed.contains(name)) read(listed.at(name), lists[static_cast<std::size_t>(list)]);
    if (answer.contains("banned")) read(answer.at("banned"), lists[banned]);
    if (answer.contains("official_servers")) read(answer.at("official_servers"), lists[servers], server_id);
    if (answer.contains("blocked_servers")) read(answer.at("blocked_servers"), lists[blocked], any_server_id);
    return lists;
}
bool parse_server_tokens_required(std::string_view json) {
    const auto answer = Json::parse(json);
    return answer.is_object() && answer.contains("server_tokens_required") && answer.at("server_tokens_required").is_boolean() &&
           answer.at("server_tokens_required").get<bool>();
}
bool publish_identity_lists(IdentityLists lists) {
    const auto previous = current.load();
    if (previous && *previous == lists) return false;
    current.store(std::make_shared<const IdentityLists>(std::move(lists)));
    return true;
}
bool identity_listed(std::uint64_t id, IdentityList list) noexcept {
    const auto lists = current.load();
    if (!id || !lists || list >= IdentityList::count) return false;
    const auto &ids = (*lists)[static_cast<std::size_t>(list)];
    return std::binary_search(ids.begin(), ids.end(), id);
}
} // namespace dingosdk::multiplayer
