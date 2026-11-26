#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// Short-lived replacement for one real controller. No plugin code runs on input threads.
namespace dingosdk::input {
struct PadOutput {
    std::uint16_t buttons{};
    std::uint8_t left_trigger{}, right_trigger{};
    std::int16_t left_x{}, left_y{}, right_x{}, right_y{};
};
class PadOutputs {
public:
    static constexpr unsigned playstation = 4;
    static constexpr std::uint64_t lifetime_ms = 100;
    std::string write(std::string_view owner, unsigned controller, std::optional<PadOutput> pad, std::uint64_t now,
        bool* changed = nullptr) {
        if (changed) *changed = false;
        if (controller > playstation) return "controller is 0 to 3, or playstation";
        std::lock_guard lock(mutex_);
        auto& lease = leases_[controller];
        if (!pad) {
            if (!lease.owner.empty() && lease.owner == owner) { lease = {}; if (changed) *changed = true; }
            return {};
        }
        if (!lease.owner.empty() && lease.owner != owner && now < lease.expires)
            return lease.owner + " replaces this controller";
        lease = {std::string(owner), *pad, now + lifetime_ms};
        if (changed) *changed = true;
        return {};
    }
    std::optional<PadOutput> read(unsigned controller, std::uint64_t now, bool allowed, bool* changed = nullptr) {
        if (changed) *changed = false;
        std::lock_guard lock(mutex_);
        // Never replay an old gesture after a menu closes or focus returns.
        if (!allowed) {
            for (auto& lease : leases_) {
                if (changed && !lease.owner.empty()) *changed = true;
                lease = {};
            }
            return std::nullopt;
        }
        if (controller > playstation) return std::nullopt;
        auto& lease = leases_[controller];
        if (lease.owner.empty()) return std::nullopt;
        if (now >= lease.expires) { lease = {}; if (changed) *changed = true; return std::nullopt; }
        return lease.pad;
    }
    void release(std::string_view owner, bool* changed = nullptr) {
        if (changed) *changed = false;
        std::lock_guard lock(mutex_);
        for (auto& lease : leases_) if (!lease.owner.empty() && lease.owner == owner) {
            lease = {}; if (changed) *changed = true;
        }
    }
    // Availability reads do not retire state or call an extension while holding this mutex.
    bool active(std::uint64_t now) {
        std::lock_guard lock(mutex_);
        for (const auto& lease : leases_) if (!lease.owner.empty() && now < lease.expires) return true;
        return false;
    }
private:
    struct Lease { std::string owner; PadOutput pad; std::uint64_t expires{}; };
    std::mutex mutex_;
    std::array<Lease, 5> leases_{};
};
inline PadOutputs& pad_outputs() { static auto* value = new PadOutputs; return *value; }
}
