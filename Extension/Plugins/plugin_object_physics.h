#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::plugins {
struct ObjectPhysics { bool dynamic{true}; float mass{80}; };
struct ObjectMotion {
    std::array<float, 3> position{}, velocity{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    bool dynamic{};
    std::uint64_t generation{}; // Opaque placement lifetime token, never an engine address.
};
struct MarkerEvents { std::vector<std::string> events; bool overflow{}; };
inline bool valid_object_physics(const ObjectPhysics& value) {
    return std::isfinite(value.mass) && value.mass >= 1 && value.mass <= 10000;
}
inline bool valid_object_velocity(const std::array<float, 3>& velocity) {
    float squared{};
    for (float value : velocity) {
        if (!std::isfinite(value)) return false;
        squared += value * value;
    }
    return squared <= 45 * 45;
}
inline bool valid_object_rotation(const std::array<float, 4>& rotation) {
    float norm{};
    for (float value : rotation) { if (!std::isfinite(value)) return false; norm += value * value; }
    return norm > .95f && norm < 1.05f;
}
// Process-wide arbitration is independent of native handles. A plugin cannot
// change or release another plugin's claim, even if it knows the layout ID.
class ObjectPhysicsClaims {
    std::map<std::uint64_t, std::string> owners_;
public:
    bool occupied(std::uint64_t id) const { return owners_.contains(id); }
    bool owns(std::string_view plugin, std::uint64_t id) const {
        const auto at = owners_.find(id);
        return at != owners_.end() && at->second == plugin;
    }
    std::string claim(std::string_view plugin, std::uint64_t id) {
        if (!id || plugin.empty()) return "invalid physics owner or object ID";
        if (auto at = owners_.find(id); at != owners_.end())
            return at->second == plugin ? std::string{} : "another plugin controls this object's physics";
        std::size_t count{};
        for (const auto& [key, owner] : owners_) { (void)key; if (owner == plugin) ++count; }
        if (count >= 32 || owners_.size() >= 128) return "object physics limit reached";
        owners_.emplace(id, plugin);
        return {};
    }
    void erase(std::uint64_t id) { owners_.erase(id); }
    void clear() { owners_.clear(); }
};
// Native callbacks enqueue opaque owner tokens; polling returns only local
// operation names. Each plugin consumes independently, with bounded history.
class MarkerJournal {
    struct Entry { std::uint64_t sequence{}, owner{}; bool saved{}; };
    std::deque<Entry> history_;
    std::map<std::string, std::uint64_t, std::less<>> cursors_;
    std::uint64_t sequence_{};
public:
    void push(std::uint64_t owner, bool saved) {
        if (!owner) return;
        if (history_.size() >= 32) history_.pop_front();
        history_.push_back({++sequence_, owner, saved});
    }
    std::optional<MarkerEvents> poll(std::string_view plugin, std::uint64_t local_owner) {
        auto at = cursors_.find(plugin);
        if (at == cursors_.end()) {
            if (cursors_.size() >= 128) return std::nullopt;
            cursors_.emplace(plugin, sequence_);
            return MarkerEvents{};
        }
        MarkerEvents result;
        result.overflow = !history_.empty() && at->second < history_.front().sequence - 1;
        for (const auto& entry : history_)
            if (entry.sequence > at->second && local_owner && entry.owner == local_owner)
                result.events.emplace_back(entry.saved ? "saved" : "returned");
        at->second = sequence_;
        return result;
    }
    void release(std::string_view plugin) { cursors_.erase(std::string(plugin)); }
    void reset() {
        history_.clear();
        for (auto& [plugin, cursor] : cursors_) { (void)plugin; cursor = sequence_; }
    }
};
}
