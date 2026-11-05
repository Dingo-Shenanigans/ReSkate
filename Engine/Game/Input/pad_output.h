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
    std::string write(std::string_view owner, unsigned controller, std::optional<PadOutput> pad, std::uint64_t now) {
        if (controller > playstation) return "controller is 0 to 3, or playstation";
        std::lock_guard lock(mutex_);
        auto& lease = leases_[controller];
        if (!pad) { if (lease.owner == owner) lease = {}; return {}; }
        if (!lease.owner.empty() && lease.owner != owner && now < lease.expires)
            return lease.owner + " replaces this controller";
        lease = {std::string(owner), *pad, now + lifetime_ms};
        return {};
    }
    std::optional<PadOutput> read(unsigned controller, std::uint64_t now, bool allowed) {
        std::lock_guard lock(mutex_);
        // Never replay an old gesture after a menu closes or focus returns.
        if (!allowed) { for (auto& lease : leases_) lease = {}; return std::nullopt; }
        if (controller > playstation) return std::nullopt;
        auto& lease = leases_[controller];
        if (lease.owner.empty()) return std::nullopt;
        if (now >= lease.expires) { lease = {}; return std::nullopt; }
        return lease.pad;
    }
    void release(std::string_view owner) {
        std::lock_guard lock(mutex_);
        for (auto& lease : leases_) if (lease.owner == owner) lease = {};
    }
private:
    struct Lease { std::string owner; PadOutput pad; std::uint64_t expires{}; };
    std::mutex mutex_;
    std::array<Lease, 5> leases_{};
};
inline PadOutputs& pad_outputs() { static auto* value = new PadOutputs; return *value; }
}
