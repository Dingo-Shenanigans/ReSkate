#include "world_pool_usage.h"

#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/entity_pages.h"
#include "Engine/Game/Build/20260929/spatial_buckets.h"
#include "Engine/Game/Build/20260929/world_pool_usage.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace dingosdk {
namespace {
namespace contract = addr::world_pool_usage;
namespace pages = addr::entity_pages;
namespace spatial = addr::spatial_buckets;

// What the hooks have seen: snapshot page arrays and spatial roots, kept for the watcher to read.
// A snapshot or root that is freed later stays listed; its reads then fail or find it zeroed.
template<std::size_t N>
struct Seen {
    std::array<std::atomic<std::uintptr_t>, N> items{};
    void add(std::uintptr_t value) noexcept {
        if (!value) return;
        for (auto& item : items) {
            auto current = item.load(std::memory_order_relaxed);
            if (current == value) return;
            if (!current && item.compare_exchange_strong(current, value)) return;
            if (current == value) return;
        }
    }
};
Seen<64> snapshots;
Seen<64> roots;

using PageAllocator = void (*)(void*, void*, std::uint64_t*);
using SpatialQuery = void (*)(void*, void*, void*, void*);
std::atomic<PageAllocator> original_allocator{};
std::atomic<SpatialQuery> original_query{};

void hooked_allocator(void* owner, void* allocator, std::uint64_t* page_array) {
    original_allocator.load(std::memory_order_relaxed)(owner, allocator, page_array);
    snapshots.add(reinterpret_cast<std::uintptr_t>(page_array));
}
// (Both callers walk the roots and drop the result, so none is passed back.)
void hooked_query(void* root, void* a, void* b, void* c) {
    roots.add(reinterpret_cast<std::uintptr_t>(root));
    original_query.load(std::memory_order_relaxed)(root, a, b, c);
}

bool matches(std::uintptr_t address, const auto& expected) noexcept {
    std::array<std::uint8_t, 32> actual{};
    return expected.size() <= actual.size() && memory::read_bytes(address, actual.data(), expected.size()) &&
        std::equal(expected.begin(), expected.end(), actual.begin());
}

// The highest entity slot in use in one snapshot: its last page with anything in it, and the last
// entry there that is not zero. Pages are read from the top down: the unused ones are zero.
std::size_t entity_peak(std::uintptr_t page_array, std::vector<std::uint64_t>& page) noexcept {
    for (std::size_t index = pages::page_count; index-- > 0;) {
        std::uintptr_t address{};
        if (!memory::peek(page_array + index * sizeof(std::uint64_t), address) || !address) continue;
        if (!memory::peek_bytes(address, page.data(), pages::page_bytes)) continue;
        for (std::size_t entry = pages::page_entries; entry-- > 0;)
            if (page[entry]) return index * pages::page_entries + entry + 1;
    }
    return 0;
}

std::size_t spatial_peak(std::uintptr_t root) noexcept {
    std::uint32_t nodes{};
    if (!memory::peek(root + offsetof(spatial::RootLayout, node_count), nodes) || nodes > spatial::nodes_per_root) return 0;
    std::size_t most{};
    for (std::uint32_t node = 0; node < nodes; ++node) {
        std::uint32_t blocks{};
        const auto at = root + contract::spatial_nodes_offset + node * sizeof(spatial::BucketLayout) +
            offsetof(spatial::BucketLayout, block_count);
        if (memory::peek(at, blocks) && blocks <= spatial::blocks_per_bucket) most = std::max<std::size_t>(most, blocks);
    }
    return most;
}

std::size_t render_instance_peak(std::uintptr_t base) noexcept {
    std::uintptr_t owner{}, manager{};
    std::uint32_t counter{};
    if (!memory::peek(base + contract::render_instance_owner_global, owner) || !owner ||
        !memory::peek(owner + contract::render_instance_manager_offset, manager) || !manager ||
        !memory::peek(manager + contract::render_instance_pool_offset + contract::render_instance_counter_offset, counter))
        return 0;
    return counter;
}

// A new high is said when it has grown by a sixteenth of the game's own size since the last, and
// whenever it first passes the game's own size.
struct Peak {
    const char* name;
    std::size_t stock;
    std::size_t capacity;
    std::size_t said{};
    bool past{};
    void see(std::size_t value) {
        if (value <= said) return;
        const bool now_past = value > stock;
        if (value < said + stock / 16 && now_past == past) return;
        said = value;
        logging::log(now_past && !past ? logging::Level::warning : logging::Level::info, logging::Channel::world,
            "World pools: {} {} of ReSkate's {} ({:.0f}% of the game's own {}){}", name, value, capacity, 100.0 * value / stock, stock,
            now_past && !past ? ": past the game's own size" : "");
        past = now_past;
    }
};

void watch(std::uintptr_t base) {
    SetThreadDescription(GetCurrentThread(), L"ReSkate world pool watch");
    std::vector<std::uint64_t> page(pages::page_entries);
    Peak entities{"entity slots", contract::entity_slots_stock, pages::capacity};
    Peak instances{"render instances", contract::render_instances_stock, std::size_t{512} << pages::render_instance_page_shift};
    Peak blocks{"spatial blocks in a bucket", contract::spatial_blocks_stock, spatial::blocks_per_bucket};
    for (;;) {
        Sleep(1000);
        try {
            std::size_t entity_most{}, block_most{};
            for (const auto& item : snapshots.items)
                if (const auto value = item.load(std::memory_order_relaxed)) entity_most = std::max(entity_most, entity_peak(value, page));
            for (const auto& item : roots.items)
                if (const auto value = item.load(std::memory_order_relaxed)) block_most = std::max(block_most, spatial_peak(value));
            entities.see(entity_most);
            instances.see(render_instance_peak(base));
            blocks.see(block_most);
        } catch (...) {}
    }
}
} // namespace

bool start_world_pool_usage(std::uintptr_t base, std::string& error) {
    wchar_t value[2]{};
    if (GetEnvironmentVariableW(L"RESKATE_GPU_DIAGNOSTICS", value, 2) != 1 || value[0] != L'1') return true;
    if (!matches(base + contract::entity_page_allocator_rva, contract::entity_page_allocator_entry) ||
        !matches(base + contract::spatial_query_rva, contract::spatial_query_entry) ||
        !matches(base + contract::render_instance_load_rva, contract::render_instance_load) ||
        !matches(base + contract::render_instance_counter_rva, contract::render_instance_counter)) {
        error = "the world pools do not match the supported game";
        return false;
    }
    auto* const allocator = reinterpret_cast<void*>(base + contract::entity_page_allocator_rva);
    auto* const query = reinterpret_cast<void*>(base + contract::spatial_query_rva);
    void* allocator_original{};
    void* query_original{};
    if (hook_prepare(allocator, reinterpret_cast<void*>(&hooked_allocator), &allocator_original) != HookOk ||
        hook_prepare(query, reinterpret_cast<void*>(&hooked_query), &query_original) != HookOk) {
        error = "its hooks could not be prepared";
        return false;
    }
    original_allocator.store(reinterpret_cast<PageAllocator>(allocator_original));
    original_query.store(reinterpret_cast<SpatialQuery>(query_original));
    if (hook_enable(allocator) != HookOk || hook_enable(query) != HookOk) {
        error = "its hooks could not be attached";
        return false;
    }
    std::thread(watch, base).detach();
    return true;
}
} // namespace dingosdk
