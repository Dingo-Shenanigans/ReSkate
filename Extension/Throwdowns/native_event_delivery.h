#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>

namespace dingosdk::multiplayer {
// Observation still runs for held native outcomes. Only events actually delivered
// to the native graph may occupy its ordered sender-attribution queue.
inline constexpr bool hold_native_skate_send(std::uint32_t event, bool detector_active, bool detector_injected) {
    return detector_active && !detector_injected && (event == 0xe85319b1 || event == 0xb2fc396c);
}

class NativeEventSendQueue {
public:
    struct Sent { std::uint32_t event, sender; std::uint64_t ms; };

    void note(std::uint32_t event, std::uint32_t sender, std::uint64_t now, bool delivered = true) {
        if (!delivered) return;
        // Preserve ReSkate's native queue expiry, capacity and first-handler order.
        while (!sent_.empty() && now - sent_.front().ms > 5000) sent_.pop_front();
        if (sent_.size() < 256) sent_.push_back({event, sender, now});
    }
    template<class Matches> std::optional<Sent> first(Matches matches) const {
        const auto it = std::find_if(sent_.begin(), sent_.end(), matches);
        return it == sent_.end() ? std::nullopt : std::optional<Sent>{*it};
    }
    template<class Matches> std::optional<Sent> take(Matches matches) {
        const auto it = std::find_if(sent_.begin(), sent_.end(), matches);
        if (it == sent_.end()) return {};
        const auto result = *it;
        sent_.erase(it);
        return result;
    }
    bool has_injected() const {
        return std::any_of(sent_.begin(), sent_.end(), [](const auto& sent) { return sent.sender != 0; });
    }
private:
    std::deque<Sent> sent_;
};
} // namespace dingosdk::multiplayer
