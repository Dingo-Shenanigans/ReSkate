#include "game_fatal_dump.h"

#include "Engine/Core/Debug/crash_report.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/game_fatal_dump.h"
#include "Extension/UI/Overlay/gpu_diagnostics.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace dingosdk {
namespace {
namespace contract = addr::game_fatal_dump;
using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
std::atomic<WriteDump> original_write{};
std::uintptr_t installed_base{};
std::mutex install_mutex;

// Only a dump of this process with an exception to show is a crash; the game's dump follows
// whether or not the reporter took one. (Its pointers are this process's either way.)
BOOL WINAPI write_dump(HANDLE process, DWORD process_id, HANDLE file, MINIDUMP_TYPE type,
    PMINIDUMP_EXCEPTION_INFORMATION exception, PMINIDUMP_USER_STREAM_INFORMATION streams,
    PMINIDUMP_CALLBACK_INFORMATION callback) {
    if (exception && exception->ExceptionPointers && process_id == GetCurrentProcessId()) {
        // (A GPU that stopped: the next launches record what it was doing.)
        if (const auto* record = exception->ExceptionPointers->ExceptionRecord)
            overlay::note_gpu_stopped(record->ExceptionCode);
        crash_report::capture_game_fatal(exception->ThreadId, exception->ExceptionPointers);
    }
    return original_write.load(std::memory_order_acquire)(process, process_id, file, type, exception, streams, callback);
}

// The slot holds the export it was bound to: MiniDumpWriteDump of the module that address is in.
bool bound_to_minidump(std::uintptr_t value) noexcept {
    HMODULE module{};
    if (!value || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(value), &module)) return false;
    return reinterpret_cast<std::uintptr_t>(GetProcAddress(module, "MiniDumpWriteDump")) == value;
}
} // namespace

bool start_game_fatal_dump(std::uintptr_t base, std::string& error) {
    std::lock_guard lock(install_mutex);
    error.clear();
    if (installed_base) {
        if (installed_base == base) return true;
        error = "already installed on another image";
        return false;
    }
    std::array<std::uint8_t, contract::thunk_bytes.size()> thunk{};
    std::uintptr_t bound{};
    if (!memory::read_bytes(base + contract::thunk, thunk.data(), thunk.size()) ||
        !std::equal(thunk.begin(), thunk.end(), contract::thunk_bytes.begin()) ||
        !memory::read(base + contract::import_slot, bound) || !bound_to_minidump(bound)) {
        error = "the game's MiniDumpWriteDump import does not match the supported game";
        return false;
    }
    original_write.store(reinterpret_cast<WriteDump>(bound), std::memory_order_release);
    auto* const slot = reinterpret_cast<void**>(base + contract::import_slot);
    DWORD previous{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) {
        error = "its import slot could not be made writable";
        return false;
    }
    InterlockedExchangePointer(slot, reinterpret_cast<void*>(&write_dump));
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), previous, &ignored);
    installed_base = base;
    return true;
}
} // namespace dingosdk
