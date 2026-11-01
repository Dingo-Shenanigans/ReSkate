#include "native_type_scan.h"
#include <Windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstring>

// Data-defined (EBX) types are not in the engine's static type list and have no
// by-hash lookup, but their reflection records live in ordinary private memory:
//   record:  {u32 nameHash, u16 flags (kind in bits 5-9), u16 size, ...,
//             +0x20 -> the "array of T" type object, ..., +0x2a u16 fieldCount,
//             +0x60 -> field table}
//   object:  {record*, ...}
// and the array type's record keeps its element type object at +0x30. So a type
// object is recovered from its record as read(read(read(record+0x20))+0x30),
// which must point back at the record -- a check no stray bytes will pass.
namespace dingosdk::multiplayer {
namespace {
bool readable(const MEMORY_BASIC_INFORMATION& info) noexcept {
    return info.State == MEM_COMMIT && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (info.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
}
// No C++ objects with destructors in here: structured exception handling guards
// against a page being released while it is read.
bool copy_guarded(std::uintptr_t address, void* out, std::size_t size) noexcept {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> bool get(std::uintptr_t address, T& value) noexcept {
    return address >= 0x10000 && address < 0x00007fffffff0000ULL && copy_guarded(address, &value, sizeof(value));
}
std::uintptr_t object_of(std::uintptr_t record) noexcept {
    std::uintptr_t array_object{}, array_record{}, object{}, back{};
    if (!get(record + 0x20, array_object) || !get(array_object, array_record) || !get(array_record + 0x30, object) ||
        !get(object, back) || back != record) return 0;
    return object;
}
std::size_t scan_region(std::uintptr_t begin, std::size_t size, NativeTypeQuery* wanted, std::size_t count) noexcept {
    std::size_t found{};
    __try {
        const auto* words = reinterpret_cast<const std::uint32_t*>(begin);
        for (std::size_t i = 0; i + 2 <= size / 4; i += 2) { // records are 8-byte aligned
            const auto hash = words[i];
            for (std::size_t q = 0; q < count; ++q) {
                auto& query = wanted[q];
                if (query.object || hash != query.hash) continue;
                const auto flags = static_cast<std::uint16_t>(words[i + 1]);
                if (((flags >> 5) & 0x1f) != query.kind) continue;
                if (const auto object = object_of(begin + i * 4)) { query.object = object; ++found; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}
// Whether QueryWorkingSetEx reports resident pages at all. Under Wine on macOS
// (CrossOver), NtQueryVirtualMemory(MemoryWorkingSetExInformation) has no
// /proc/self/pagemap to read and succeeds with Valid = 0 for every page, even
// for the stack page just written here.
bool working_set_supported() noexcept {
    static const bool supported = []() noexcept {
        PSAPI_WORKING_SET_EX_INFORMATION page{&page};
        return QueryWorkingSetEx(GetCurrentProcess(), &page, sizeof(page)) && page.VirtualAttributes.Valid;
    }();
    return supported;
}
// Scans only the pages of a region that are in the working set. Reading any other
// committed page makes the kernel fault it in: a zero page never touched, or one
// paged out to disk. Type records are written when registered, so they are
// resident unless the system is short of memory (then a scan may miss them).
std::size_t scan_resident(std::uintptr_t begin, std::size_t size, NativeTypeQuery* wanted, std::size_t count) noexcept {
    constexpr std::size_t page = 4096, batch = 512;
    PSAPI_WORKING_SET_EX_INFORMATION pages[batch];
    std::size_t found{};
    for (std::uintptr_t at = begin; at < begin + size;) {
        const auto n = std::min(batch, (begin + size - at) / page);
        if (!n) break;
        for (std::size_t i = 0; i < n; ++i) pages[i].VirtualAddress = reinterpret_cast<void*>(at + i * page);
        if (!QueryWorkingSetEx(GetCurrentProcess(), pages, static_cast<DWORD>(n * sizeof(pages[0])))) {
            found += scan_region(at, n * page, wanted, count);
        } else {
            for (std::size_t i = 0; i < n;) {
                if (!pages[i].VirtualAttributes.Valid) { ++i; continue; }
                auto end = i;
                while (end < n && pages[end].VirtualAttributes.Valid) ++end;
                found += scan_region(at + i * page, (end - i) * page, wanted, count);
                i = end;
            }
        }
        at += n * page;
    }
    return found;
}
} // namespace

std::size_t find_native_types(std::span<NativeTypeQuery> wanted, bool background) noexcept {
    std::size_t missing{};
    for (const auto& query : wanted) missing += query.object ? 0 : 1;
    // A worker's scan reads a large share of the process's memory: at background
    // CPU, I/O and memory priority, yielding between regions, it competes as
    // little as possible with the game's threads for cores and memory bandwidth.
    const bool lowered = background && SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    const bool resident_only = background && working_set_supported();
    std::uintptr_t at = 0x10000;
    MEMORY_BASIC_INFORMATION info{};
    while (missing && at < 0x00007fffffff0000ULL && VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info))) {
        const auto base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        // Type records are heap data: private, writable, and never in huge blocks.
        if (readable(info) && info.Type == MEM_PRIVATE && (info.Protect & PAGE_READWRITE) && info.RegionSize <= (1ULL << 30)) {
            missing -= resident_only ? scan_resident(base, info.RegionSize, wanted.data(), wanted.size())
                                     : scan_region(base, info.RegionSize, wanted.data(), wanted.size());
            if (background) SwitchToThread();
        }
        at = base + info.RegionSize;
    }
    if (lowered) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    return missing;
}
}
