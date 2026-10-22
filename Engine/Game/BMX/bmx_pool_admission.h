#pragma once
#include <cstdint>
#include <span>

namespace dingosdk::bmx::pool_admission {
struct WorldKey {
    std::uintptr_t world{}, state{}, context{};
    bool operator==(const WorldKey&) const = default;
    bool valid() const noexcept { return world && state && context; }
};
struct Owner {
    std::uintptr_t client{}, entity{}, core{}, context{}, rig{};
    bool operator==(const Owner&) const = default;
    bool valid() const noexcept { return client && entity && core && context && rig; }
};
enum class Decision { proceed, wait, refused };
struct Result {
    Decision decision = Decision::wait;
    const char* issue = "BMX waiting for fresh physics ownership.";
};
struct PoolView {
    WorldKey current;
    Owner owner;
    bool attempted{}, ready{}, active{}, detached{}, failed{}, blocked{}, producer_failed{};
    std::span<const WorldKey> attempted_worlds;
};
// Admission only: no handle validation, allocation, adoption or cleanup is
// authorized by proceed. Acquisition must repeat its complete native proof.
inline Result evaluate(const PoolView& pool, const WorldKey& fresh, const Owner& owner) noexcept {
    if (!fresh.valid() || !owner.valid()) return {};
    if (fresh != pool.current) {
        for (const auto& attempted : pool.attempted_worlds) {
            if (attempted == fresh) return {Decision::refused,
                "BMX physics was already attempted in this world. Restart this isolated game before mounting again."};
        }
        // Published producer failure can belong to an abandoned older world.
        // A never-attempted full key keeps the existing acquire/reset path.
        return {Decision::proceed, "BMX physics mount admission ready."};
    }
    if (!pool.owner.valid()) return {};
    if (pool.owner != owner) return {Decision::refused,
        "BMX physics ownership changed with the local rider. Restart this isolated game before mounting again."};
    if (pool.blocked || pool.failed || pool.producer_failed || (pool.attempted && !pool.ready))
        return {Decision::refused,
            "BMX physics is unavailable in this world. Restart this isolated game before mounting again."};
    if (pool.active || pool.detached) return {Decision::wait,
        "BMX waiting for the existing bike to finish its current transition."};
    return {Decision::proceed, "BMX physics mount admission ready."};
}
} // namespace dingosdk::bmx::pool_admission
