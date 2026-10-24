#include "custom_modes.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace dingosdk::multiplayer::custom_throwdowns {
namespace {
struct Registry {
    std::mutex mutex;
    std::vector<Mode> modes;
    std::atomic<std::uint64_t> revision{1};
};
Registry& registry() { static auto* r = new Registry; return *r; }
} // namespace

std::string set_mode(Mode mode) {
    if (mode.plugin.empty() || mode.id.empty() || mode.id.size() > 32 || mode.id.find('/') != std::string::npos)
        return "a mode needs an id of 1 to 32 characters, without /";
    if (mode.name.empty() || mode.name.size() > 32) return "a mode needs a name of 1 to 32 characters";
    if (mode.description.size() > 240) return "a description is at most 240 characters";
    // "spot_battle" is what plugins said before the kit named the stock cards.
    if (mode.replaces == "spot_battle") mode.replaces = "spot";
    if (!mode.replaces.empty() && mode.replaces != "skate" && mode.replaces != "spot" && mode.replaces != "jam")
        return "replaces is \"skate\", \"spot\" or \"jam\"";
    if (mode.card.empty()) mode.card = "spot";
    if (mode.card == "spot_battle") mode.card = "spot";
    if (mode.card != "skate" && mode.card != "spot" && mode.card != "jam") return "card is \"skate\", \"spot\" or \"jam\"";
    if (mode.art.size() > 256 || mode.flag.size() > 256) return "art and flag are at most 256 characters";
    if (mode.min_players < 1 || mode.max_players > 10 || mode.min_players > mode.max_players)
        return "players are 1 to 10, min no more than max";
    if (!std::isfinite(mode.cursor_height) || mode.cursor_height < 0 || mode.cursor_height > 100000)
        return "cursor_height is 0 to 100000 metres";
    mode.players = std::clamp(mode.players, mode.min_players, mode.max_players);
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    const auto key = mode.key();
    const auto found = std::find_if(r.modes.begin(), r.modes.end(), [&](const Mode& m) { return m.key() == key; });
    if (found == r.modes.end() && r.modes.size() >= 32) return "32 modes are registered already";
    if (found != r.modes.end()) *found = std::move(mode);
    else r.modes.push_back(std::move(mode));
    r.revision.fetch_add(1);
    return {};
}
void remove_mode(std::string_view plugin, std::string_view id) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    if (std::erase_if(r.modes, [&](const Mode& m) { return m.plugin == plugin && m.id == id; })) r.revision.fetch_add(1);
}
void remove_plugin(std::string_view plugin) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    if (std::erase_if(r.modes, [&](const Mode& m) { return m.plugin == plugin; })) r.revision.fetch_add(1);
}
std::vector<Mode> modes() {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    return r.modes;
}
std::optional<Mode> find(std::string_view key) {
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    for (const auto& mode : r.modes)
        if (mode.key() == key) return mode;
    return std::nullopt;
}
std::uint64_t revision() noexcept { return registry().revision.load(); }
} // namespace dingosdk::multiplayer::custom_throwdowns
