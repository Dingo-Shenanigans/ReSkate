#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// Temporary boolean gameplay preferences. Values are observed at the game's settings getter;
// profile writes and the game's Settings screen always retain the player's own preferences.
namespace dingosdk::input {
class GameplayOptions {
public:
    bool observe(std::string_view name, bool original, std::uint64_t now, bool editing) {
        std::lock_guard lock(mutex_);
        auto& entry = entries_[std::string(name)]; entry.original = original;
        if (!editing && !entry.owner.empty() && now < entry.expires) return entry.value;
        if (now >= entry.expires) entry.owner.clear();
        return original;
    }
    std::optional<bool> get(std::string_view name) {
        std::lock_guard lock(mutex_);
        const auto at = entries_.find(name);
        return at == entries_.end() ? std::nullopt : std::optional<bool>{at->second.original};
    }
    std::string hold(std::string_view owner, std::string_view name, std::optional<bool> value, std::uint64_t now) {
        std::lock_guard lock(mutex_);
        const auto at = entries_.find(name);
        if (at == entries_.end()) return "this boolean gameplay option has not been read by the game yet";
        auto& e = at->second;
        if (!value) { if (e.owner == owner) e.owner.clear(); return {}; }
        if (!e.owner.empty() && e.owner != owner && now < e.expires) return e.owner + " holds this gameplay option";
        std::size_t held{};
        for (const auto& [key, entry] : entries_) { (void)key; if (entry.owner == owner && now < entry.expires) ++held; }
        if ((e.owner != owner || now >= e.expires) && held >= 32) return "at most 32 gameplay options per plugin";
        e.owner = owner; e.value = *value; e.expires = now + 300;
        return {};
    }
    void release(std::string_view owner) {
        std::lock_guard lock(mutex_);
        for (auto& [name, e] : entries_) { (void)name; if (e.owner == owner) e.owner.clear(); }
    }
private:
    struct Entry { bool original{}, value{}; std::string owner; std::uint64_t expires{}; };
    std::mutex mutex_;
    std::map<std::string, Entry, std::less<>> entries_;
};
inline GameplayOptions& gameplay_options() { static auto* value = new GameplayOptions; return *value; }
}
