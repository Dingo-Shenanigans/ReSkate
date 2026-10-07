#include "developer_identity.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <memory>
#include <stdexcept>

// The lists themselves, shared by the game and the dedicated server. Each reads
// them from the backend its own way: developer_identity_fetch.cpp, Server/global_bans.cpp.
namespace dingosdk::multiplayer {
namespace {
// The API's name for each category, in IdentityList order; the ban list follows them.
constexpr std::array<std::string_view, 4> categories{"dev", "homie", "content_creator", "centrix"};
constexpr auto banned = static_cast<std::size_t>(IdentityList::banned);

std::atomic<std::shared_ptr<const IdentityLists>> current;

// A player's SteamID64 is 76561197960265728 plus a 32-bit account number, and
// nobody has account 0. The API sends them as strings.
std::uint64_t steam_id(const Json &entry) {
    constexpr std::uint64_t base = 76561197960265728ULL, last = base + 0xffffffffULL;
    std::uint64_t id{};
    if (entry.is_string()) {
        const auto &text = entry.string();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
        if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && id > base && id <= last) return id;
    }
    throw std::runtime_error("an entry is not a player's SteamID64");
}
} // namespace

IdentityLists parse_identity_lists(std::string_view json) {
    const auto answer = Json::parse(json);
    if (!answer.is_object() || !answer.contains("categories") || !answer.at("categories").is_object())
        throw std::runtime_error("the answer has no categories");
    const auto &listed = answer.at("categories");
    IdentityLists lists;
    const auto read = [](const Json &entries, std::vector<std::uint64_t> &ids) {
        if (!entries.is_array()) throw std::runtime_error("a list of players is not a list");
        for (const auto &entry : entries) ids.push_back(steam_id(entry));
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    };
    for (std::size_t index = 0; index < categories.size(); ++index)
        if (listed.contains(categories[index])) read(listed.at(categories[index]), lists[index]);
    if (answer.contains("banned")) read(answer.at("banned"), lists[banned]);
    return lists;
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
