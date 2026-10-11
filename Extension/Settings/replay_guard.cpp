#include "replay_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/replay_recorder.h"
#include <Windows.h>
#include <array>
#include <atomic>

namespace dingosdk::replay_guard {
namespace {
namespace rr = addr::replay_recorder;
// (Two arguments are read; the others are passed on as they came.)
using EndChunk = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t);
EndChunk original{};
std::uintptr_t fault_at{};
std::atomic<bool> installed{};
std::atomic<std::uint64_t> skips{};

// Only the write to the chunk head the stream did not give: nothing else is this guard's.
int ours(const EXCEPTION_POINTERS* info) noexcept {
    const auto& record = *info->ExceptionRecord;
    return record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
           reinterpret_cast<std::uintptr_t>(record.ExceptionAddress) == fault_at
               ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
// Said the first time and then now and again: it can happen every few frames once it starts.
__declspec(noinline) void note_skip() noexcept {
    const auto count = skips.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count != 1 && count % 1000) return;
    try {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Replay recording: the game had no room for its next chunk and would have crashed; the chunk was skipped "
            "({} so far). Replays of this stretch may have gaps.", count);
    } catch (...) {}
}
std::uint64_t guarded(void* recorder, std::uint64_t force, std::uint64_t third, std::uint64_t fourth) {
    __try {
        return original(recorder, force, third, fourth);
    } __except (ours(GetExceptionInformation())) {
        note_skip();
        return 0; // no chunk was ended
    }
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return original != nullptr;
    if (!base) return false;
    std::array<unsigned char, 32> bytes{};
    if (!memory::peek(base + rr::end_chunk.rva, bytes) || bytes != rr::end_chunk.bytes) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Replay guard: this build's replay recorder differs; it is left as it is.");
        return false;
    }
    fault_at = base + rr::end_chunk_head_store;
    const auto target = reinterpret_cast<void*>(base + rr::end_chunk.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&guarded), reinterpret_cast<void**>(&original)) != HookOk ||
        hook_enable(target) != HookOk) {
        original = nullptr;
        logging::log(logging::Level::warning, logging::Channel::runtime, "Replay guard: the replay recorder could not be hooked.");
        return false;
    }
    return true;
}

std::uint64_t skipped() noexcept { return skips.load(std::memory_order_relaxed); }
} // namespace dingosdk::replay_guard
