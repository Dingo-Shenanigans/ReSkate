#include "streaming_copy_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/streaming_reads.h"
#include <array>
#include <atomic>
#include <bit>

namespace dingosdk::streaming_copy_guard {
namespace {
namespace sr = addr::streaming_reads;
using Finish = void (*)(std::uintptr_t, std::uintptr_t);
Finish original{};
std::atomic<bool> installed{};
std::atomic<std::uint64_t> pieces{};

// Said for the first few, then each power of two: one bad asset is read again each time it streams.
__declspec(noinline) void note(const sr::Place& place) noexcept {
    const auto count = pieces.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > 4 && !std::has_single_bit(count)) return;
    try {
        logging::log(logging::Level::warning, logging::Channel::assets,
            "Streaming: a read asked for {} x {} bytes from offset {:#x} of its 64 KB staging buffer, where the game would have "
            "crashed; that piece is left unfilled ({} piece(s) since the game started). A mod's asset whose header does not "
            "match its data does this.", place.count, place.size, place.offset, count);
    } catch (...) {}
}

// Whether `place` reads only inside the staging buffer: `count` pieces of `size` from `offset` on.
bool inside(const sr::Place& place) noexcept {
    // (The game copies a place at least once, whatever its count.)
    const std::uint64_t count = place.count ? place.count : 1;
    return std::uint64_t{place.offset} + std::uint64_t{place.size} * count <= sr::staging_size;
}

void guarded(std::uintptr_t scheduler, std::uintptr_t request) {
    std::uint32_t flags{};
    std::uintptr_t places{};
    if (request && memory::peek(request + sr::flags_offset, flags) && (flags & sr::staged_flags) &&
        memory::peek(request + sr::places_offset, places) && places) {
        const auto total = (flags >> sr::place_count_shift) & sr::place_count_mask;
        auto* const list = reinterpret_cast<sr::Place*>(places);
        for (std::uint32_t i = 0; i < total; ++i) {
            sr::Place place{};
            if (!memory::peek(places + std::uintptr_t{i} * sizeof(sr::Place), place)) break;
            if (inside(place)) continue;
            note(place);
            // A size of nothing: the game's loop still goes round `count` times, copying nothing.
            list[i].size = 0;
        }
    }
    original(scheduler, request);
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return original != nullptr;
    if (!base) return false;
    std::array<unsigned char, 32> head{};
    if (!memory::peek(base + sr::finish.rva, head) || head != sr::finish.bytes) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Streaming copy guard: this build's streaming code differs; it is left as it is.");
        return false;
    }
    const auto target = reinterpret_cast<void*>(base + sr::finish.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&guarded), reinterpret_cast<void**>(&original)) != HookOk ||
        hook_enable(target) != HookOk) {
        original = nullptr;
        logging::log(logging::Level::warning, logging::Channel::runtime, "Streaming copy guard: the game's function could not be hooked.");
        return false;
    }
    return true;
}

std::uint64_t emptied() noexcept { return pieces.load(std::memory_order_relaxed); }
} // namespace dingosdk::streaming_copy_guard
