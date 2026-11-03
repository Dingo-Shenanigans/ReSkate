#include "outfit_filter.h"
#include "Engine/Core/Json/json.h"
#include <algorithm>
#include <charconv>

// The rules alone, with nothing of the game: outfit_filter.cpp keeps them, the tests read them.
namespace dingosdk::multiplayer::outfit_filter {
namespace {
constexpr std::size_t maximum_rules_bytes = 1024 * 1024, maximum_entries = 4096, maximum_key = 255;
std::string lower(std::string_view text) {
    std::string result(text);
    for (auto &ch : result)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return result;
}
} // namespace

bool hides(const Rules &rules, std::uint64_t player, std::string_view key, const ItemFacts &facts) {
    if (key.empty()) return false;
    if (rules.items.contains(lower(key))) return true;
    if (!facts.mod.empty() && rules.mods.contains(facts.mod)) return true;
    // The game's own items are only hidden by name: a rule about mods leaves them be.
    if (facts.game) return false;
    return !rules.mod_items || rules.players.contains(player);
}

std::string readable_key(std::string_view key) {
    while (key.size() > 4 && lower(key.substr(0, 4)) == "own_") key.remove_prefix(4);
    std::string out;
    for (const char c : key) {
        const char shown = c == '_' ? ' ' : c;
        if (shown == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += shown;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? std::string(key) : out;
}

std::string readable_mod(std::string_view folder) {
    const auto dash = folder.find('-');
    const auto name = dash == std::string_view::npos || dash + 1 == folder.size() ? folder : folder.substr(dash + 1);
    std::string out;
    for (const char c : name) {
        const char shown = c == '_' ? ' ' : c;
        if (shown == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += shown;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? std::string(folder) : out;
}

std::string to_json(const Rules &rules) {
    auto root = Json::object();
    root["schema"] = 1;
    root["mod_items"] = rules.mod_items;
    root["items"] = Json(std::vector<std::string>(rules.items.begin(), rules.items.end()));
    root["mods"] = Json(std::vector<std::string>(rules.mods.begin(), rules.mods.end()));
    auto players = Json::object();
    // Steam IDs are 64-bit: as text, which every JSON reader keeps whole.
    for (const auto &[id, name] : rules.players) players[std::to_string(id)] = name;
    root["players"] = std::move(players);
    return root.dump(2) + "\n";
}

Rules from_json(std::string_view text) noexcept {
    Rules rules;
    try {
        const auto root = Json::parse(text, {.bytes = maximum_rules_bytes});
        if (!root.is_object()) return {};
        if (root.contains("mod_items") && root.at("mod_items").is_boolean()) rules.mod_items = root.at("mod_items").get<bool>();
        const auto names = [&](std::string_view field, std::set<std::string, std::less<>> &out) {
            if (!root.contains(field) || !root.at(field).is_array()) return;
            for (const auto &value : root.at(field))
                if (value.is_string() && !value.string().empty() && value.string().size() <= maximum_key && out.size() < maximum_entries)
                    out.insert(lower(value.string()));
        };
        names("items", rules.items);
        names("mods", rules.mods);
        if (root.contains("players") && root.at("players").is_object())
            for (const auto &[id, name] : root.at("players").items()) {
                std::uint64_t value{};
                if (std::from_chars(id.data(), id.data() + id.size(), value).ec != std::errc{} || !value) continue;
                if (rules.players.size() < maximum_entries) rules.players[value] = name.is_string() ? name.string().substr(0, 64) : std::string();
            }
    } catch (...) {
        return {};
    }
    return rules;
}
} // namespace dingosdk::multiplayer::outfit_filter
