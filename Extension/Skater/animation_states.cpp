#include "Extension/Skater/animation_states.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <atomic>
#include <mutex>
#include <string>

namespace dingosdk::animation_states {
namespace {
using logging::Channel;
using logging::Level;

struct State {
    std::mutex mutex;
    animation_values::Found found;       // under mutex
    animation_values::SearchRule rule;   // under mutex
    std::uint64_t generation{};          // under mutex: counts the levels, so a search of the last one is not kept
    std::atomic<bool> searching{};
    // What the search thread goes by.
    std::uintptr_t base{};
    std::uint64_t search_generation{};
};
State& state() {
    static State s;
    return s;
}

// Every writable region of the game's memory that is not an image, as the trainer's own search goes through them
// (trainer_classes.cpp): the game's assets are loaded into such memory.
DWORD WINAPI search(void*) noexcept {
    auto& s = state();
    try {
        const auto started = GetTickCount64();
        animation_values::Found found;
        std::uint64_t bytes = 0;
        MEMORY_BASIC_INFORMATION info{};
        for (std::uintptr_t at = 0x10000; at < 0x7fffffff0000ull && VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == sizeof(info);
             at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize) {
            if (info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE || info.Type == MEM_IMAGE) continue;
            found.add_block(s.base, reinterpret_cast<std::uintptr_t>(info.BaseAddress), info.RegionSize);
            bytes += info.RegionSize;
        }
        const auto names = found.descriptors.size();
        bool kept = false;
        {
            std::scoped_lock lock(s.mutex);
            if (s.generation == s.search_generation) {
                s.found = std::move(found);
                kept = true;
            }
        }
        logging::log(Level::info, Channel::skater, "Animation values: {} named values found in {} MB of the game's memory in {} ms{}.", names,
                     bytes >> 20, GetTickCount64() - started, kept ? "" : " (for a level that has gone: not kept)");
    } catch (...) {
    }
    s.searching.store(false, std::memory_order_release);
    return 0;
}
}

std::optional<animation_values::Value> read(std::uintptr_t base, std::uintptr_t core, std::string_view name) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    return s.found.read(base, core, name);
}
bool known(std::string_view name) {
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    return s.found.knows(name);
}
void wanted(std::uintptr_t base, std::string_view level, bool missing, std::uint64_t now_ms) {
    auto& s = state();
    {
        std::scoped_lock lock(s.mutex);
        if (s.rule.new_level(level, now_ms)) {
            s.found = {};
            ++s.generation;
        }
        if (!s.rule.due(missing, s.searching.load(std::memory_order_acquire), now_ms)) return;
        s.searching.store(true, std::memory_order_release);
        s.base = base;
        s.search_generation = s.generation;
    }
    const auto thread = CreateThread(nullptr, 0, search, nullptr, 0, nullptr);
    if (thread) {
        SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(thread);
    } else {
        s.searching.store(false, std::memory_order_release);
    }
}
}
