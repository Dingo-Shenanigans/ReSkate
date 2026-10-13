#include "mesh_unload_guard.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/mesh_variations.h"
#include <Windows.h>
#include <array>
#include <atomic>

namespace dingosdk::mesh_unload_guard {
namespace {
namespace mv = addr::mesh_variations;
// (Three arguments are read; the fourth is passed on as it came.)
using RemoveDatabases = std::uint64_t (*)(void*, void*, void*, std::uint64_t);
RemoveDatabases original{};
std::uintptr_t fault_at{}, resume_at{};
std::atomic<bool> installed{};
std::atomic<std::uint64_t> skips{};

// Only the read of a mesh that is gone (a null one: rax is the entry's mesh): the function goes
// on at its step to the next entry, as if that entry had been taken out. Nothing else is this
// guard's.
int ours(EXCEPTION_POINTERS* info) noexcept {
    const auto& record = *info->ExceptionRecord;
    if (record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION || reinterpret_cast<std::uintptr_t>(record.ExceptionAddress) != fault_at ||
        info->ContextRecord->Rax != 0)
        return EXCEPTION_CONTINUE_SEARCH;
    info->ContextRecord->Rip = resume_at;
    skips.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_EXECUTION;
}
// Said once per unload that had any, with how many: a map can have hundreds.
__declspec(noinline) void note_skips(std::uint64_t before) noexcept {
    const auto now = skips.load(std::memory_order_relaxed);
    if (now == before) return;
    try {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Level unload: {} mesh variation(s) had lost their mesh before their database was unloaded, where the game would "
            "have crashed; they were passed over ({} since the game started).", now - before, now);
    } catch (...) {}
}
std::uint64_t guarded(void* registry, void* second, void* list, std::uint64_t fourth) {
    const auto before = skips.load(std::memory_order_relaxed);
    std::uint64_t result{};
    __try {
        result = original(registry, second, list, fourth);
    } __except (ours(GetExceptionInformation())) {
        // (Never entered: the filter either goes on in the function or passes the fault up.)
    }
    note_skips(before);
    return result;
}
} // namespace

bool install(std::uintptr_t base) noexcept {
    if (installed.exchange(true)) return original != nullptr;
    if (!base) return false;
    std::array<unsigned char, 32> head{};
    std::array<unsigned char, 6> read{};
    std::array<unsigned char, 4> next{};
    if (!memory::peek(base + mv::remove_databases.rva, head) || head != mv::remove_databases.bytes ||
        !memory::peek(base + mv::mesh_hash_read, read) || read != mv::mesh_hash_read_bytes ||
        !memory::peek(base + mv::next_entry, next) || next != mv::next_entry_bytes) {
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Mesh unload guard: this build's mesh variation code differs; it is left as it is.");
        return false;
    }
    fault_at = base + mv::mesh_hash_read;
    resume_at = base + mv::next_entry;
    const auto target = reinterpret_cast<void*>(base + mv::remove_databases.rva);
    if (hook_prepare(target, reinterpret_cast<void*>(&guarded), reinterpret_cast<void**>(&original)) != HookOk ||
        hook_enable(target) != HookOk) {
        original = nullptr;
        logging::log(logging::Level::warning, logging::Channel::runtime, "Mesh unload guard: the game's function could not be hooked.");
        return false;
    }
    return true;
}

std::uint64_t skipped() noexcept { return skips.load(std::memory_order_relaxed); }
} // namespace dingosdk::mesh_unload_guard
