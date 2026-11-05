#include "patches.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace dingosdk {
namespace {
struct Entry {
    PatchRecord record;
    void* original{};
    void* replacement{};
    bool interlocked{};
};
struct Registry {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<Entry> entries;
};
Registry& registry() {
    // Never freed: a breakpoint handler may restore its site during process teardown.
    static auto* value = new Registry;
    return *value;
}
struct Exclusive {
    SRWLOCK& lock;
    explicit Exclusive(SRWLOCK& l) noexcept : lock(l) { AcquireSRWLockExclusive(&lock); }
    ~Exclusive() { ReleaseSRWLockExclusive(&lock); }
    Exclusive(const Exclusive&) = delete;
    Exclusive& operator=(const Exclusive&) = delete;
};
HookStatus status(LONG error) { return static_cast<HookStatus>(error); }
HookStatus last_error() {
    const auto error = static_cast<LONG>(GetLastError());
    return status(error ? error : static_cast<LONG>(ERROR_ACCESS_DENIED));
}

Entry* find(Registry& r, std::uintptr_t address) noexcept {
    const auto found = std::find_if(r.entries.begin(), r.entries.end(),
        [&](const Entry& entry) { return entry.record.address == address; });
    return found == r.entries.end() ? nullptr : &*found;
}
// A patch that was written stays written when its record cannot be kept (out of memory).
void record(const Entry& entry) noexcept {
    auto& r = registry();
    Exclusive lock(r.lock);
    if (auto* existing = find(r, entry.record.address)) { *existing = entry; return; }
    try { r.entries.push_back(entry); } catch (const std::bad_alloc&) {}
}
// Allocation-free, so a breakpoint handler can call it.
void deactivate(std::uintptr_t address) noexcept {
    auto& r = registry();
    Exclusive lock(r.lock);
    if (auto* existing = find(r, address)) existing->record.active = false;
}

bool read_memory(std::uintptr_t address, void* destination, std::size_t size) noexcept {
    SIZE_T count{};
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), destination,
        size, &count) && count == size;
}
bool holds(std::uintptr_t address, std::span<const std::uint8_t> expected) noexcept {
    std::array<std::uint8_t, 64> actual{};
    for (std::size_t done = 0; done < expected.size(); done += actual.size()) {
        const auto size = std::min(actual.size(), expected.size() - done);
        if (!read_memory(address + done, actual.data(), size) ||
            std::memcmp(actual.data(), expected.data() + done, size) != 0) return false;
    }
    return true;
}
// The steps every code patch took before it came here; the old protection is put back even when
// the flush fails, and either failure is reported.
HookStatus write_code(std::uintptr_t address, std::span<const std::uint8_t> bytes) noexcept {
    auto* const destination = reinterpret_cast<void*>(address);
    DWORD previous{};
    if (!VirtualProtect(destination, bytes.size(), PAGE_EXECUTE_READWRITE, &previous)) return last_error();
    std::memcpy(destination, bytes.data(), bytes.size());
    const auto flushed = FlushInstructionCache(GetCurrentProcess(), destination, bytes.size()) ? HookOk : last_error();
    DWORD discarded{};
    if (!VirtualProtect(destination, bytes.size(), previous, &discarded)) return last_error();
    return flushed;
}

HookStatus swap_slot(const char* kind, const char* name, void** slot, void* expected, void* replacement,
    void** original, bool interlocked) noexcept {
    if (original) *original = nullptr;
    void* current{};
    if (!slot || !read_memory(reinterpret_cast<std::uintptr_t>(slot), &current, sizeof(current)) || current != expected)
        return HookBytesDiffer;
    DWORD previous{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) return last_error();
    if (interlocked) InterlockedExchangePointer(slot, replacement);
    else *static_cast<void* volatile*>(slot) = replacement;
    // As before, a failure to put the protection back does not undo the swap.
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), previous, &ignored);
    if (original) *original = expected;
    record({{kind, name, reinterpret_cast<std::uintptr_t>(slot), sizeof(void*), true}, expected, replacement, interlocked});
    return HookOk;
}
} // namespace

HookStatus patch_table_slot(const char* name, void** slot, void* expected, void* replacement, void** original) noexcept {
    return swap_slot("table slot", name, slot, expected, replacement, original, false);
}
HookStatus patch_import_slot(const char* name, void** slot, void* expected, void* replacement, void** original) noexcept {
    return swap_slot("import slot", name, slot, expected, replacement, original, true);
}
HookStatus patch_slot_restore(void** slot) noexcept {
    Entry entry{};
    {
        auto& r = registry();
        Exclusive lock(r.lock);
        const auto* existing = find(r, reinterpret_cast<std::uintptr_t>(slot));
        if (!existing || !existing->record.active || existing->record.size != sizeof(void*)) return HookNotFound;
        entry = *existing;
    }
    void* current{};
    if (!read_memory(entry.record.address, &current, sizeof(current)) || current != entry.replacement)
        return HookBytesDiffer;
    DWORD previous{};
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous)) return last_error();
    if (entry.interlocked) InterlockedExchangePointer(slot, entry.original);
    else *static_cast<void* volatile*>(slot) = entry.original;
    DWORD ignored{};
    VirtualProtect(slot, sizeof(void*), previous, &ignored);
    deactivate(entry.record.address);
    return HookOk;
}

bool code_jump_bytes(std::uintptr_t at, std::uintptr_t destination, std::span<std::uint8_t> out) noexcept {
    if (out.size() < 5) return false;
    const auto delta = static_cast<std::int64_t>(destination) - static_cast<std::int64_t>(at + 5);
    if (delta < std::numeric_limits<std::int32_t>::min() || delta > std::numeric_limits<std::int32_t>::max()) return false;
    const auto displacement = static_cast<std::int32_t>(delta);
    std::fill(out.begin(), out.end(), std::uint8_t{0x90});
    out[0] = 0xe9;
    std::memcpy(out.data() + 1, &displacement, sizeof(displacement));
    return true;
}
HookStatus patch_code(const char* name, std::uintptr_t address, std::span<const std::uint8_t> expected,
    std::span<const std::uint8_t> replacement) noexcept {
    if (!address || expected.empty() || expected.size() != replacement.size() || !holds(address, expected))
        return HookBytesDiffer;
    const auto result = write_code(address, replacement);
    // A failed write may still have changed the bytes; the caller restores them, as it did before.
    if (result == HookOk) record({{"code", name, address, replacement.size(), true}});
    return result;
}
HookStatus patch_code_jump(const char* name, std::uintptr_t address, std::span<const std::uint8_t> expected,
    std::uintptr_t destination) noexcept {
    std::array<std::uint8_t, 64> jump{};
    if (expected.size() < 5 || expected.size() > jump.size()) return HookBytesDiffer;
    const auto bytes = std::span{jump}.first(expected.size());
    if (!code_jump_bytes(address, destination, bytes)) return HookOutOfReach;
    return patch_code(name, address, expected, bytes);
}
HookStatus patch_code_restore(std::uintptr_t address, std::span<const std::uint8_t> original) noexcept {
    if (!address || original.empty()) return HookBytesDiffer;
    const auto result = write_code(address, original);
    if (result == HookOk) deactivate(address);
    return result;
}

HookStatus patch_breakpoint(const char* name, std::uintptr_t address, std::uint8_t opcode) noexcept {
    const std::array<std::uint8_t, 1> expected{opcode}, breakpoint{0xcc};
    if (!address || !holds(address, expected)) return HookBytesDiffer;
    const auto result = write_code(address, breakpoint);
    if (result == HookOk) record({{"breakpoint", name, address, 1, true}});
    return result;
}
HookStatus patch_breakpoint_restore(std::uintptr_t address, std::uint8_t opcode) noexcept {
    const std::array<std::uint8_t, 1> original{opcode};
    return patch_code_restore(address, original);
}
void* patch_breakpoint_handler(const char* name, PVECTORED_EXCEPTION_HANDLER handler) noexcept {
    auto* const handle = AddVectoredExceptionHandler(1, handler);
    if (handle) record({{"breakpoint handler", name, reinterpret_cast<std::uintptr_t>(handle), 0, true}});
    return handle;
}
void patch_breakpoint_handler_remove(void* handle) noexcept {
    if (!handle) return;
    RemoveVectoredExceptionHandler(handle);
    deactivate(reinterpret_cast<std::uintptr_t>(handle));
}

std::vector<PatchRecord> patch_records() {
    auto& r = registry();
    Exclusive lock(r.lock);
    std::vector<PatchRecord> result;
    result.reserve(r.entries.size());
    for (const auto& entry : r.entries) result.push_back(entry.record);
    return result;
}
}
