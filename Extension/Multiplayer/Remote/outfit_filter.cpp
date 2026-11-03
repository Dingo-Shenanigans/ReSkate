#include "outfit_filter.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Vfs/mod_catalog.h"
#include "Extension/Customization/item_origins.h"
#include "Extension/Multiplayer/Net/protocol.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <mutex>

namespace dingosdk::multiplayer::outfit_filter {
namespace {
namespace fs = std::filesystem;
// Players whose outfits are remembered for the menu: everyone in a full session, and some who left.
constexpr std::size_t remembered_players = 64;

struct State {
    std::mutex mutex;
    std::shared_ptr<const Rules> rules;
    std::map<std::uint64_t, std::pair<ULONGLONG, std::vector<WornItem>>> worn;
    std::set<std::string, std::less<>> seen;
};
State &state() {
    static auto *value = new State; // process lifetime
    return *value;
}
std::atomic<std::uint64_t> current_revision{1};
std::string lower(std::string text) {
    for (auto &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}
fs::path rules_path() { return mods::engine_data_root() / mods::mods_folder / rules_file; }

Rules load() {
    std::ifstream input(rules_path(), std::ios::binary);
    if (!input) return {};
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return from_json(text);
}
} // namespace

std::shared_ptr<const Rules> rules() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    if (!s.rules) s.rules = std::make_shared<const Rules>(load());
    return s.rules;
}

std::uint64_t revision() noexcept { return current_revision.load(std::memory_order_acquire); }

bool set_rules(Rules value) {
    const auto text = to_json(value);
    {
        auto &s = state();
        std::lock_guard lock(s.mutex);
        s.rules = std::make_shared<const Rules>(std::move(value));
    }
    current_revision.fetch_add(1, std::memory_order_acq_rel);
    try {
        const auto target = rules_path();
        auto temporary = target;
        temporary += L".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output || !output.write(text.data(), static_cast<std::streamsize>(text.size())) || !output.flush()) return false;
        }
        if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        std::error_code ignored;
        fs::remove(temporary, ignored);
    } catch (...) {
    }
    logging::log(logging::Level::warning, logging::Channel::runtime, "Outfit filter: {} could not be saved.", rules_file);
    return false;
}

ItemFacts facts(std::string_view key) {
    ItemFacts out;
    out.game = item_origins::game_item(key);
    // The rules name a mod by its folder in lower case; a variant's items are its mod's
    // ("<folder>#<variant>", mod_catalog.h).
    if (const auto origin = item_origins::find(key)) {
        out.mod = origin->folder.substr(0, origin->folder.find('#'));
        for (auto &c : out.mod) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

void note_worn(std::uint64_t player, std::vector<WornItem> items) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    for (const auto &item : items)
        if (!item.mod.empty()) s.seen.insert(item.mod);
    s.worn[player] = {GetTickCount64(), std::move(items)};
    while (s.worn.size() > remembered_players) {
        auto oldest = s.worn.begin();
        for (auto at = s.worn.begin(); at != s.worn.end(); ++at)
            if (at->second.first < oldest->second.first) oldest = at;
        s.worn.erase(oldest);
    }
}

std::vector<WornItem> worn(std::uint64_t player) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    const auto found = s.worn.find(player);
    return found == s.worn.end() ? std::vector<WornItem>{} : found->second.second;
}

std::set<std::string, std::less<>> take_seen_mods() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return std::exchange(s.seen, {});
}

std::vector<Wanted> wanted() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    std::vector<Wanted> out;
    std::set<std::string, std::less<>> listed;
    for (const auto &[player, noted] : s.worn)
        for (const auto &item : noted.second)
            if (!item.installed && !item.hidden && !item.package.empty() && listed.insert(lower(item.package)).second)
                out.push_back({item.package, item.key, player});
    return out;
}

std::vector<ItemSource> item_sources(const std::vector<std::string> &keys) {
    std::vector<ItemSource> out;
    for (const auto &key : keys) {
        if (out.size() >= max_item_sources) break;
        if (key.empty() || item_origins::game_item(key) ||
            std::ranges::any_of(out, [&](const ItemSource &source) { return source.asset == key; }))
            continue;
        const auto origin = item_origins::find(key);
        if (!origin) continue;
        auto package = map_package_name(origin->folder.substr(0, origin->folder.find('#')), origin->version);
        if (!package.empty()) out.push_back({key, std::move(package)});
    }
    return out;
}

std::vector<ItemSource> item_sources(const Appearance &appearance) {
    std::vector<std::string> keys;
    for (const auto *recipe : {&appearance.skater, &appearance.board})
        for (const auto &slot : recipe->items) keys.push_back(slot.asset);
    return item_sources(keys);
}
} // namespace dingosdk::multiplayer::outfit_filter
