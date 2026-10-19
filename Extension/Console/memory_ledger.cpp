#include "Extension/Console/memory_ledger.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <Psapi.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dingosdk::memory_ledger {
namespace {
constexpr std::size_t depth = 14;
constexpr std::size_t caller_slots = 8192;
constexpr unsigned block_bits = 18;
constexpr std::size_t block_slots = std::size_t{1} << block_bits;
constexpr std::uint32_t gone = 0xffffffff;

struct Caller {
    std::uint64_t hash{};
    std::array<void*, depth> frames{};
    unsigned frame_count{};
    // What was asked for while `perf memory trace` was on: reservations and commits of the
    // traced sizes. smallest/largest: the sizes of those.
    std::uint64_t asked_count{}, asked_bytes{}, smallest{}, largest{};
};
// A reservation the process still holds and who made it. caller: an index into callers plus
// one; 0 is an empty slot, `gone` one whose reservation was given back (kept so a search
// for one further along still finds it).
struct Block {
    std::uintptr_t base{};
    std::uint32_t caller{};
    std::uint32_t made{}; // seconds after the ledger started
};

// Never held while allocating: the heap may grow and come back through the hook on the same
// thread. What is copied out under it goes into storage allocated before.
// (None of this is ever destroyed: the hooks stay on while the process exits, and what other
// modules give back then still passes them.)
std::mutex& mutex = *new std::mutex;
std::vector<Caller>& callers = *new std::vector<Caller>(caller_slots);
std::vector<Block>& blocks = *new std::vector<Block>(block_slots);
std::size_t blocks_live{}, blocks_gone{};
std::uint64_t untracked{}; // reservations the tables had no room for
std::atomic<bool> running{}, counting{};
std::atomic<std::uint64_t> least{1}, most{UINT64_MAX};
std::atomic<std::uint64_t> began{};
std::mutex& start_mutex = *new std::mutex;
bool prepared{};

using Allocate = LONG (NTAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
using AllocateEx = LONG (NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG, ULONG, void*, ULONG);
using Free = LONG (NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG);
Allocate original{};
AllocateEx original_ex{};
Free original_free{};

std::size_t block_slot(std::uintptr_t base) noexcept {
    return static_cast<std::size_t>(((base >> 16) * 0x9e3779b97f4a7c15ull) >> (64 - block_bits));
}
// Under the lock. The search is bounded: a hook cannot take long.
void remember(std::uintptr_t base, std::uint32_t caller, std::uint32_t made) noexcept {
    std::size_t free_at = block_slots;
    for (std::size_t probe = 0; probe < 2048; ++probe) {
        const auto at = (block_slot(base) + probe) & (block_slots - 1);
        auto& block = blocks[at];
        if (!block.caller) { if (free_at == block_slots) free_at = at; break; }
        if (block.caller == gone) { if (free_at == block_slots) free_at = at; continue; }
        // (A release that was not seen: the same base is made again.)
        if (block.base == base) { block.caller = caller; block.made = made; return; }
    }
    if (free_at == block_slots || (!blocks[free_at].caller && (blocks_live + blocks_gone) * 8 >= block_slots * 7)) {
        ++untracked;
        return;
    }
    if (blocks[free_at].caller == gone) --blocks_gone;
    blocks[free_at] = {base, caller, made};
    ++blocks_live;
}
void forget(std::uintptr_t base) noexcept {
    for (std::size_t probe = 0; probe < 2048; ++probe) {
        auto& block = blocks[(block_slot(base) + probe) & (block_slots - 1)];
        if (!block.caller) return;
        if (block.caller != gone && block.base == base) {
            block.caller = gone;
            --blocks_live;
            ++blocks_gone;
            return;
        }
    }
}
const Block* find(const std::vector<Block>& table, std::uintptr_t base) noexcept {
    for (std::size_t probe = 0; probe < 2048; ++probe) {
        const auto& block = table[(block_slot(base) + probe) & (block_slots - 1)];
        if (!block.caller) return nullptr;
        if (block.caller != gone && block.base == base) return &block;
    }
    return nullptr;
}
// Under the lock: the caller's index plus one, made if new; 0 when the table is full.
std::uint32_t caller_of(std::uint64_t hash, const std::array<void*, depth>& frames, unsigned count) noexcept {
    for (std::size_t probe = 0; probe < 128; ++probe) {
        const auto at = (hash + probe) % caller_slots;
        auto& caller = callers[at];
        if (caller.hash && caller.hash != hash) continue;
        if (!caller.hash) {
            caller = {};
            caller.hash = hash;
            caller.frames = frames;
            caller.frame_count = count;
        }
        return static_cast<std::uint32_t>(at + 1);
    }
    return 0;
}

// This process, by its pseudo-handle or a real handle to it (which some code opens for itself).
bool own(HANDLE process) noexcept {
    return process == GetCurrentProcess() || (process && GetProcessId(process) == GetCurrentProcessId());
}

// (Not inlined: the stack is captured from a known depth, two frames under the caller of
// NtAllocateVirtualMemory.)
__declspec(noinline) void note(HANDLE process, void* base, SIZE_T asked, ULONG type) noexcept {
    if (!running.load(std::memory_order_relaxed) || !(type & (MEM_COMMIT | MEM_RESERVE)) || !own(process))
        return;
    const bool reserve = (type & MEM_RESERVE) != 0;
    const bool counted = counting.load(std::memory_order_relaxed) && asked >= least.load(std::memory_order_relaxed) &&
                         asked <= most.load(std::memory_order_relaxed);
    if (!reserve && !counted) return; // (a commit inside a reservation that is already noted)
    std::array<void*, depth> frames{};
    const unsigned count = CaptureStackBackTrace(2, static_cast<DWORD>(depth), frames.data(), nullptr);
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned i = 0; i < count; ++i) hash = hash * 1099511628211ull ^ reinterpret_cast<std::uintptr_t>(frames[i]);
    if (!hash) hash = 1;
    const auto made = static_cast<std::uint32_t>((GetTickCount64() - began.load(std::memory_order_relaxed)) / 1000);
    std::lock_guard lock(mutex);
    const auto index = caller_of(hash, frames, count);
    if (!index) { if (reserve) ++untracked; return; }
    if (counted) {
        auto& caller = callers[index - 1];
        caller.smallest = caller.asked_count ? std::min<std::uint64_t>(caller.smallest, asked) : asked;
        caller.largest = std::max<std::uint64_t>(caller.largest, asked);
        ++caller.asked_count;
        caller.asked_bytes += asked;
    }
    if (reserve && base) remember(reinterpret_cast<std::uintptr_t>(base), index, made);
}
LONG NTAPI hooked(HANDLE process, PVOID* base, ULONG_PTR zero_bits, PSIZE_T size, ULONG type, ULONG protect) {
    const auto asked = size ? *size : 0;
    const auto status = original(process, base, zero_bits, size, type, protect);
    if (status >= 0) note(process, base ? *base : nullptr, asked, type);
    return status;
}
// The newer call for the same (VirtualAlloc2, which graphics drivers and the D3D12 runtime use).
LONG NTAPI hooked_ex(HANDLE process, PVOID* base, PSIZE_T size, ULONG type, ULONG protect, void* parameters, ULONG parameter_count) {
    const auto asked = size ? *size : 0;
    const auto status = original_ex(process, base, size, type, protect, parameters, parameter_count);
    if (status >= 0) note(process, base ? *base : nullptr, asked, type);
    return status;
}
LONG NTAPI hooked_free(HANDLE process, PVOID* base, PSIZE_T size, ULONG type) {
    const auto at = base ? reinterpret_cast<std::uintptr_t>(*base) : 0;
    const auto status = original_free(process, base, size, type);
    // (A placeholder split or joined is still reserved: MEM_COALESCE/PRESERVE_PLACEHOLDER.)
    if (status >= 0 && at && (type & MEM_RELEASE) && !(type & 0x3) && running.load(std::memory_order_relaxed) &&
        own(process)) {
        std::lock_guard lock(mutex);
        forget(at);
    }
    return status;
}

void* ntdll_function(const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name));
}

std::string megabytes(std::uint64_t bytes) {
    return bytes >= (1ull << 30) ? std::format("{:.2f} GB", static_cast<double>(bytes) / (1ull << 30))
                                 : std::format("{:.1f} MB", static_cast<double>(bytes) / (1ull << 20));
}
std::string age(std::uint32_t seconds) {
    return seconds < 120 ? std::format("{} s", seconds) : seconds < 7200 ? std::format("{} min", seconds / 60)
                                                                         : std::format("{:.1f} h", seconds / 3600.0);
}

// ---- reading what the process holds ----
struct Range {
    std::uintptr_t base{};
    std::uint64_t bytes{};
    DWORD protect{};
};
struct Allocation {
    std::uint64_t committed{}, reserved{}; // reserved: the whole reservation, committed or not
    DWORD reserved_as{};                   // the protection it was reserved with
    DWORD committed_as{};                  // every protection its committed parts have, together
    std::vector<Range> ranges;             // what of it is committed (kept only when asked for)
};
struct Held {
    std::map<std::uintptr_t, Allocation> private_memory;
    std::uint64_t committed{}, mapped{}, image{};
};
Held read_held(bool keep_ranges) {
    Held held;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto at = reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress);
    const auto end = reinterpret_cast<std::uintptr_t>(system.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION info{};
    while (at < end && VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof(info)) == sizeof(info)) {
        if (info.State != MEM_FREE) {
            if (info.Type == MEM_PRIVATE) {
                auto& allocation = held.private_memory[reinterpret_cast<std::uintptr_t>(info.AllocationBase)];
                allocation.reserved += info.RegionSize;
                allocation.reserved_as = info.AllocationProtect;
                if (info.State == MEM_COMMIT) {
                    allocation.committed += info.RegionSize;
                    allocation.committed_as |= info.Protect;
                    held.committed += info.RegionSize;
                    if (keep_ranges)
                        allocation.ranges.push_back({reinterpret_cast<std::uintptr_t>(info.BaseAddress), info.RegionSize, info.Protect});
                }
            } else if (info.State == MEM_COMMIT) {
                (info.Type == MEM_IMAGE ? held.image : held.mapped) += info.RegionSize;
            }
        }
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return held;
}

struct Copy {
    std::vector<Caller> callers;
    std::vector<Block> blocks;
    std::uint64_t untracked{};
    std::size_t live{};
};
Copy copy_tables() {
    Copy copy;
    copy.callers.resize(caller_slots);
    copy.blocks.resize(block_slots);
    std::lock_guard lock(mutex);
    std::copy(callers.begin(), callers.end(), copy.callers.begin());
    std::copy(blocks.begin(), blocks.end(), copy.blocks.begin());
    copy.untracked = untracked;
    copy.live = blocks_live;
    return copy;
}

// The module an address is in, by name; empty when it is in none.
struct Module {
    std::string name;
    std::uintptr_t base{};
};
Module module_at(void* address) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) || !module)
        return {};
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; ++c) if (*c == L'\\') name = c + 1;
    Module result{{}, reinterpret_cast<std::uintptr_t>(module)};
    for (; *name; ++name) result.name += static_cast<char>(*name < 128 ? *name : '?');
    return result;
}
std::string place(void* address) {
    const auto module = module_at(address);
    if (module.name.empty()) return std::format("{:#x}", reinterpret_cast<std::uintptr_t>(address));
    return std::format("{}+{:#x}", module.name, reinterpret_cast<std::uintptr_t>(address) - module.base);
}
std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return text;
}
// Who a caller's memory belongs to: the first module on its stack that is not Windows' or the
// C runtime's way to memory. Through a heap when Windows' heap asked (the heap grew for
// whoever was allocating when it ran out).
std::string owner_of(const Caller& caller) {
    static const std::array<std::string_view, 7> passing{"ntdll.dll", "kernelbase.dll", "kernel32.dll", "ucrtbase.dll",
        "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"};
    bool heap{};
    for (unsigned i = 0; i < caller.frame_count; ++i) {
        const auto module = module_at(caller.frames[i]);
        const auto name = lower(module.name);
        if (i == 0 && name == "ntdll.dll") heap = true;
        if (name.empty() || std::ranges::find(passing, name) != passing.end()) continue;
        return heap ? module.name + " (through a heap)" : module.name;
    }
    return heap ? "Windows (a heap)" : "Windows";
}
// "read/write, write-combined": what a page protection allows, in words.
std::string protection(DWORD protect) {
    std::string text;
    const auto add = [&](std::string_view word) { text += (text.empty() ? "" : "+") + std::string(word); };
    constexpr std::pair<DWORD, std::string_view> kinds[] = {{PAGE_NOACCESS, "no access"}, {PAGE_READONLY, "read"},
        {PAGE_READWRITE, "read/write"}, {PAGE_WRITECOPY, "write-copy"}, {PAGE_EXECUTE, "execute"},
        {PAGE_EXECUTE_READ, "execute/read"}, {PAGE_EXECUTE_READWRITE, "execute/read/write"},
        {PAGE_EXECUTE_WRITECOPY, "execute/write-copy"}};
    for (const auto& [bit, word] : kinds) if (protect & bit) add(word);
    if (protect & PAGE_GUARD) text += ", guard";
    if (protect & PAGE_NOCACHE) text += ", uncached";
    if (protect & PAGE_WRITECOMBINE) text += ", write-combined";
    return text.empty() ? std::format("{:#x}", protect) : text;
}

std::string stack_of(const Caller& caller) {
    std::string line;
    for (unsigned i = 0; i < caller.frame_count; ++i) line += " < " + place(caller.frames[i]);
    return line;
}

constexpr std::string_view unseen = "not seen being made (before the ledger started, or by Windows itself: thread stacks)";

// How much of an allocation is in RAM, and of a few of those pages, how many hold only zeros.
struct Use {
    std::uint64_t resident{};
    unsigned sampled{}, zero{};
};
Use use_of(const Allocation& allocation, std::vector<PSAPI_WORKING_SET_EX_INFORMATION>& pages) {
    Use use;
    const auto total_pages = allocation.committed / 4096;
    const std::uint64_t stride = std::max<std::uint64_t>(1, total_pages / 8);
    std::uint64_t page_number{};
    std::array<unsigned char, 4096> bytes{};
    for (const auto& range : allocation.ranges) {
        // (Pages that would be spoiled or slow to read are not read: a guard page would lose
        // its guard, and write-combined or uncached memory is the GPU's.)
        const bool readable = !(range.protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_WRITECOMBINE | PAGE_NOCACHE));
        for (std::uint64_t done = 0; done < range.bytes;) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(pages.size(), (range.bytes - done) / 4096));
            if (!count) break;
            for (std::size_t i = 0; i < count; ++i) pages[i].VirtualAddress = reinterpret_cast<void*>(range.base + done + i * 4096);
            if (!QueryWorkingSetEx(GetCurrentProcess(), pages.data(), static_cast<DWORD>(count * sizeof(pages[0])))) return use;
            for (std::size_t i = 0; i < count; ++i, ++page_number) {
                if (!pages[i].VirtualAttributes.Valid) continue;
                use.resident += 4096;
                if (!readable || page_number % stride || use.sampled >= 8) continue;
                SIZE_T got{};
                if (!ReadProcessMemory(GetCurrentProcess(), pages[i].VirtualAddress, bytes.data(), bytes.size(), &got) ||
                    got != bytes.size())
                    continue;
                ++use.sampled;
                use.zero += std::all_of(bytes.begin(), bytes.end(), [](unsigned char byte) { return byte == 0; });
            }
            done += count * 4096ull;
        }
    }
    return use;
}

struct Totals {
    std::uint64_t committed{}, reserved{}, resident{}, zero{}, blocks{};
    std::uint32_t first_made{UINT32_MAX}, last_made{}; // seconds after the ledger started
    std::uint64_t smallest{UINT64_MAX}, largest{};
    void add(const Allocation& allocation, const Use& use, std::uint32_t made, bool known) {
        committed += allocation.committed;
        reserved += allocation.reserved;
        resident += use.resident;
        // (An estimate: the share of the pages looked at that were all zero.)
        if (use.sampled) zero += use.resident * use.zero / use.sampled;
        ++blocks;
        smallest = std::min(smallest, allocation.committed);
        largest = std::max(largest, allocation.committed);
        if (known) first_made = std::min(first_made, made), last_made = std::max(last_made, made);
    }
};
std::string totals_text(const Totals& t) {
    return std::format("{} committed, {} in RAM, {} never used or moved out, ~{} in RAM but all zero; {} reserved in {} block(s)",
        megabytes(t.committed), megabytes(t.resident), megabytes(t.committed - std::min(t.committed, t.resident)), megabytes(t.zero),
        megabytes(t.reserved), t.blocks);
}

std::uint32_t seconds_running() noexcept {
    return static_cast<std::uint32_t>((GetTickCount64() - began.load()) / 1000);
}

// ---- the leak watcher ----
std::mutex& watch_mutex = *new std::mutex;
std::string& watched_owners = *new std::string;
struct History {
    std::array<std::uint64_t, 16> committed{};
    unsigned count{};
    std::uint64_t said_at{};
};
std::unordered_map<std::uint64_t, History>& histories = *new std::unordered_map<std::uint64_t, History>; // by caller hash; the watcher's own
std::unordered_map<std::uint64_t, std::string>& owner_names = *new std::unordered_map<std::uint64_t, std::string>;

void rebuild_if_worn() {
    {
        std::lock_guard lock(mutex);
        if (blocks_gone * 8 < block_slots) return;
    }
    std::vector<Block> fresh(block_slots);
    {
        std::lock_guard lock(mutex);
        fresh.swap(blocks);
        blocks_live = blocks_gone = 0;
        for (const auto& block : fresh)
            if (block.caller && block.caller != gone) remember(block.base, block.caller, block.made);
    }
}

void watch_once() {
    rebuild_if_worn();
    const auto held = read_held(false);
    const auto copy = copy_tables();
    std::unordered_map<std::uint32_t, std::uint64_t> by_caller;
    std::map<std::string, std::uint64_t> by_owner;
    std::uint64_t unseen_bytes{};
    for (const auto& [base, allocation] : held.private_memory) {
        if (!allocation.committed) continue;
        if (const auto* block = find(copy.blocks, base)) by_caller[block->caller] += allocation.committed;
        else unseen_bytes += allocation.committed;
    }
    const auto now = GetTickCount64();
    for (const auto& [index, committed] : by_caller) {
        const auto& caller = copy.callers[index - 1];
        auto& name = owner_names[caller.hash];
        if (name.empty()) name = owner_of(caller);
        by_owner[name] += committed;
        auto& history = histories[caller.hash];
        std::rotate(history.committed.begin(), history.committed.begin() + 1, history.committed.end());
        history.committed.back() = committed;
        history.count = std::min<unsigned>(history.count + 1, static_cast<unsigned>(history.committed.size()));
        // Growing: over the last ten minutes, by 64 MB or more, and larger at nearly every look.
        constexpr unsigned span = 10;
        if (history.count <= span || now - history.said_at < 600000) continue;
        const auto* last = history.committed.data() + history.committed.size() - span - 1;
        unsigned rises{}, falls{};
        for (unsigned i = 1; i <= span; ++i) rises += last[i] > last[i - 1], falls += last[i] < last[i - 1];
        if (last[span] < last[0] + (64ull << 20) || rises < span * 7 / 10 || falls > 1) continue;
        history.said_at = now;
        logging::log(logging::Level::warning, logging::Channel::runtime,
            "Memory keeps growing: {} has {} committed, {} more than {} min ago; made by{}", name, megabytes(last[span]),
            megabytes(last[span] - last[0]), span, stack_of(caller));
    }
    // (The callers no longer holding anything are dropped from the history.)
    std::unordered_set<std::uint64_t> holding;
    for (const auto& [index, committed] : by_caller) holding.insert(copy.callers[index - 1].hash);
    std::erase_if(histories, [&](const auto& entry) { return !holding.contains(entry.first); });
    std::vector<std::pair<std::uint64_t, std::string>> order;
    for (const auto& [name, committed] : by_owner) order.emplace_back(committed, name);
    order.emplace_back(unseen_bytes, "not seen made");
    std::ranges::sort(order, std::greater{});
    std::string line = "Committed by owner:";
    for (std::size_t i = 0; i < order.size() && i < 5; ++i)
        line += std::format("{} {} {}", i ? "," : "", order[i].second, megabytes(order[i].first));
    std::lock_guard lock(watch_mutex);
    watched_owners = std::move(line);
}

void watch() {
    SetThreadDescription(GetCurrentThread(), L"ReSkate memory ledger");
    for (;;) {
        Sleep(60000);
        try { watch_once(); } catch (...) {}
    }
}

// ---- heaps ----
struct SizeCount {
    std::uint64_t size{}, count{};
};
// Block sizes go into storage made before a heap is locked: allocating from a heap while
// walking it would change what is walked.
struct SizeTable {
    static constexpr std::size_t slots = std::size_t{1} << 16;
    std::vector<SizeCount> counts = std::vector<SizeCount>(slots);
    std::uint64_t other_count{}, other_bytes{};
    void add(std::uint64_t size) noexcept {
        for (std::size_t probe = 0; probe < 64; ++probe) {
            auto& slot = counts[((size * 0x9e3779b97f4a7c15ull >> 48) + probe) & (slots - 1)];
            if (slot.count && slot.size != size) continue;
            slot.size = size;
            ++slot.count;
            return;
        }
        ++other_count;
        other_bytes += size;
    }
};
struct HeapSeen {
    HANDLE heap{};
    std::uint64_t committed{}, busy{}, free{}, largest_free{}, busy_blocks{};
    bool whole{};
};
// (A fault in the walk is caught: a heap made without its own lock may change under it.)
void walk_heap(HeapSeen& heap, SizeTable& sizes) noexcept {
    if (!HeapLock(heap.heap)) return;
    heap.whole = true;
    __try {
        PROCESS_HEAP_ENTRY entry{};
        while (HeapWalk(heap.heap, &entry)) {
            if (entry.wFlags & PROCESS_HEAP_REGION) {
                heap.committed += entry.Region.dwCommittedSize;
            } else if (entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                heap.busy += entry.cbData;
                ++heap.busy_blocks;
                sizes.add(entry.cbData);
            } else if (!(entry.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE)) {
                heap.free += entry.cbData;
                heap.largest_free = std::max<std::uint64_t>(heap.largest_free, entry.cbData);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        heap.whole = false;
    }
    HeapUnlock(heap.heap);
}
std::mutex heap_mutex;
std::map<std::uint64_t, std::uint64_t> heap_mark; // block size -> count
bool heap_marked{};
} // namespace

bool start(std::string& error) {
    std::lock_guard start_lock(start_mutex);
    if (running.load()) return true;
    if (!prepared) {
        const auto allocate = ntdll_function("NtAllocateVirtualMemory");
        const auto release = ntdll_function("NtFreeVirtualMemory");
        if (!allocate || !release || hook_prepare(allocate, reinterpret_cast<void*>(&hooked), reinterpret_cast<void**>(&original)) != HookOk) {
            error = "the allocation hook could not be prepared";
            return false;
        }
        if (hook_prepare(release, reinterpret_cast<void*>(&hooked_free), reinterpret_cast<void**>(&original_free)) != HookOk) {
            hook_remove(allocate);
            error = "the release hook could not be prepared";
            return false;
        }
        // (Missing on an older Windows: then only the first is followed.)
        if (const auto ex = ntdll_function("NtAllocateVirtualMemoryEx");
            ex && hook_prepare(ex, reinterpret_cast<void*>(&hooked_ex), reinterpret_cast<void**>(&original_ex)) != HookOk)
            original_ex = nullptr;
        prepared = true;
    }
    began.store(GetTickCount64());
    running.store(true);
    // (The release hook first: a block noted and given back before it is on would stay noted.)
    if (hook_enable(ntdll_function("NtFreeVirtualMemory")) != HookOk || hook_enable(ntdll_function("NtAllocateVirtualMemory")) != HookOk) {
        running.store(false);
        hook_disable(ntdll_function("NtFreeVirtualMemory"));
        error = "the allocation hooks could not be attached";
        return false;
    }
    if (original_ex) hook_enable(ntdll_function("NtAllocateVirtualMemoryEx"));
    std::thread(watch).detach();
    logging::log(logging::Level::info, logging::Channel::runtime,
        "Memory ledger on: who makes each reservation of memory is noted (`perf memory owners`), and an owner whose memory keeps growing is said here.");
    return true;
}
bool on() noexcept { return running.load(); }

bool start_asking(std::uint64_t smallest, std::uint64_t largest, std::string& error) {
    if (!start(error)) return false;
    {
        std::lock_guard lock(mutex);
        for (auto& caller : callers) caller.asked_count = caller.asked_bytes = caller.smallest = caller.largest = 0;
    }
    least.store(smallest);
    most.store(largest);
    counting.store(true);
    return true;
}
void stop_asking() noexcept { counting.store(false); }
bool asking() noexcept { return counting.load(); }

void asked_report(const Line& out) {
    const auto copy = copy_tables();
    std::vector<const Caller*> seen;
    for (const auto& caller : copy.callers) if (caller.asked_count) seen.push_back(&caller);
    std::ranges::sort(seen, [](const Caller* a, const Caller* b) { return a->asked_bytes > b->asked_bytes; });
    out(std::format("Memory asked for while tracing (blocks of {} and up), by who asked ({} callers), the most first:",
        megabytes(least.load()), seen.size()));
    for (std::size_t i = 0; i < seen.size() && i < 16; ++i) {
        out(seen[i]->smallest == seen[i]->largest
                ? std::format("  {} x {} bytes = {}", seen[i]->asked_count, seen[i]->smallest, megabytes(seen[i]->asked_bytes))
                : std::format("  {} blocks of {} to {} bytes = {}", seen[i]->asked_count, seen[i]->smallest, seen[i]->largest,
                      megabytes(seen[i]->asked_bytes)));
        out("     " + stack_of(*seen[i]));
    }
}

void owners_report(const Line& out, std::size_t most_callers) {
    if (std::string error; !start(error)) { out("error: " + error); return; }
    const auto copy = copy_tables();
    const auto held = read_held(true);
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> pages(16384);
    std::unordered_map<std::uint32_t, Totals> by_caller;
    std::map<std::string, Totals> by_owner;
    Totals all, unseen_totals;
    struct Unseen { std::uintptr_t base; const Allocation* allocation; Use use; };
    std::vector<Unseen> unseen_blocks;
    for (const auto& [base, allocation] : held.private_memory) {
        if (!allocation.committed) continue;
        const auto use = use_of(allocation, pages);
        const auto* block = find(copy.blocks, base);
        all.add(allocation, use, 0, false);
        if (!block) {
            unseen_totals.add(allocation, use, 0, false);
            unseen_blocks.push_back({base, &allocation, use});
            continue;
        }
        by_caller[block->caller].add(allocation, use, block->made, true);
    }
    std::unordered_map<std::uint32_t, std::string> names;
    for (const auto& [index, totals] : by_caller) {
        auto& name = names[index] = owner_of(copy.callers[index - 1]);
        auto& owner = by_owner[name];
        owner.committed += totals.committed;
        owner.reserved += totals.reserved;
        owner.resident += totals.resident;
        owner.zero += totals.zero;
        owner.blocks += totals.blocks;
    }
    by_owner[std::string(unseen)] = unseen_totals;

    out(std::format("Memory: {} committed, {} of it in RAM; {} of files mapped in, {} of programs.", megabytes(held.committed),
        megabytes(all.resident), megabytes(held.mapped), megabytes(held.image)));
    out(std::format("Of what is committed: {} never used or moved out of RAM by Windows, and about {} in RAM but all zero.",
        megabytes(all.committed - std::min(all.committed, all.resident)), megabytes(all.zero)));
    gpu_report(out);
    out(std::format("Ledger: on for {}, {} reservations held by {} callers{}.", age(seconds_running()), copy.live,
        std::ranges::count_if(copy.callers, [](const Caller& c) { return c.hash != 0; }),
        copy.untracked ? std::format(", {} not noted (its tables were full)", copy.untracked) : std::string()));
    out("By owner, the most committed first:");
    std::vector<std::pair<std::string, const Totals*>> owners;
    for (const auto& [name, totals] : by_owner) owners.emplace_back(name, &totals);
    std::ranges::sort(owners, [](const auto& a, const auto& b) { return a.second->committed > b.second->committed; });
    for (const auto& [name, totals] : owners)
        if (totals->committed) out(std::format("  {}: {}", name, totals_text(*totals)));

    // What the memory no hook saw being made is: memory Windows or a driver maps in from the
    // kernel (a GPU's memory is write-combined or uncached) never passes NtAllocateVirtualMemory.
    std::map<std::string, Totals> unseen_by_kind;
    for (const auto& block : unseen_blocks)
        unseen_by_kind[std::format("committed {}; reserved {}", protection(block.allocation->committed_as),
            protection(block.allocation->reserved_as))].add(*block.allocation, block.use, 0, false);
    out("Not seen being made, by protection:");
    std::vector<std::pair<std::string, const Totals*>> kinds;
    for (const auto& [kind, totals] : unseen_by_kind) kinds.emplace_back(kind, &totals);
    std::ranges::sort(kinds, [](const auto& a, const auto& b) { return a.second->committed > b.second->committed; });
    for (const auto& [kind, totals] : kinds) out(std::format("  {}: {}", kind, totals_text(*totals)));
    std::ranges::sort(unseen_blocks, [](const Unseen& a, const Unseen& b) { return a.allocation->committed > b.allocation->committed; });
    out("The largest of them:");
    for (std::size_t i = 0; i < unseen_blocks.size() && i < 16; ++i) {
        const auto& block = unseen_blocks[i];
        out(std::format("  {:#x}: {} committed of {} reserved, {} in RAM{}; committed {}, reserved {}, in {} part(s)", block.base,
            megabytes(block.allocation->committed), megabytes(block.allocation->reserved), megabytes(block.use.resident),
            block.use.sampled ? std::format(" ({} of {} pages looked at all zero)", block.use.zero, block.use.sampled) : std::string(),
            protection(block.allocation->committed_as), protection(block.allocation->reserved_as), block.allocation->ranges.size()));
    }

    std::vector<std::pair<std::uint32_t, const Totals*>> order;
    for (const auto& [index, totals] : by_caller) order.emplace_back(index, &totals);
    std::ranges::sort(order, [](const auto& a, const auto& b) { return a.second->committed > b.second->committed; });
    out(std::format("By caller ({}), the most committed first:", order.size()));
    for (std::size_t i = 0; i < order.size() && i < most_callers; ++i) {
        const auto& totals = *order[i].second;
        out(std::format("  {}: {}", names[order[i].first], totals_text(totals)));
        out(std::format("      blocks of {} to {}; the newest made {} ago, the oldest {} ago", megabytes(totals.smallest),
            megabytes(totals.largest), age(seconds_running() - std::min(seconds_running(), totals.last_made)),
            age(seconds_running() - std::min(seconds_running(), totals.first_made))));
        out("     " + stack_of(copy.callers[order[i].first - 1]));
    }
}

void heap_report(const Line& out, bool mark, std::size_t most_sizes) {
    std::lock_guard report_lock(heap_mutex);
    struct Summary { DWORD size; SIZE_T allocated, committed, reserved, most; };
    static const auto heap_summary = reinterpret_cast<BOOL (WINAPI*)(HANDLE, DWORD, Summary*)>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary"));
    std::array<HANDLE, 256> heaps{};
    const auto heap_count = std::min<DWORD>(GetProcessHeaps(static_cast<DWORD>(heaps.size()), heaps.data()), static_cast<DWORD>(heaps.size()));
    SizeTable sizes;
    std::vector<HeapSeen> seen(heap_count);
    std::uint64_t total_committed{}, total_busy{}, total_free{};
    std::size_t partial{};
    for (DWORD i = 0; i < heap_count; ++i) {
        auto& heap = seen[i];
        heap.heap = heaps[i];
        walk_heap(heap, sizes);
        partial += !heap.whole;
        // (A heap whose segments the walk does not show, such as one of the segment heap's: its
        // summary says what it holds.)
        if (Summary summary{sizeof(Summary)}; heap_summary && heap_summary(heaps[i], 0, &summary) && summary.committed > heap.committed)
            heap.committed = summary.committed;
        total_committed += heap.committed;
        total_busy += heap.busy;
        total_free += heap.free;
    }
    out(std::format("Heaps: {} committed in {} heap(s), {} of it in use, {} free inside them{}.", megabytes(total_committed),
        heap_count, megabytes(total_busy), megabytes(total_free),
        partial ? std::format(" ({} could not be walked whole)", partial) : std::string()));
    std::ranges::sort(seen, [](const HeapSeen& a, const HeapSeen& b) { return a.committed > b.committed; });
    for (std::size_t i = 0; i < seen.size() && i < 8; ++i) {
        if (!seen[i].committed) continue;
        out(std::format("  heap {:#x}{}: {} committed, {} in use in {} blocks, {} free (the largest free piece {})",
            reinterpret_cast<std::uintptr_t>(seen[i].heap), seen[i].heap == GetProcessHeap() ? " (the process heap)" : "",
            megabytes(seen[i].committed), megabytes(seen[i].busy), seen[i].busy_blocks, megabytes(seen[i].free),
            megabytes(seen[i].largest_free)));
    }
    std::map<std::uint64_t, std::uint64_t> now;
    for (const auto& slot : sizes.counts) if (slot.count) now[slot.size] = slot.count;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> order(now.begin(), now.end()); // size, count
    std::ranges::sort(order, [](const auto& a, const auto& b) { return a.first * a.second > b.first * b.second; });
    out("Blocks in use by size, the sizes that add up to most first:");
    for (std::size_t i = 0; i < order.size() && i < most_sizes; ++i)
        out(std::format("  {} x {} bytes = {}", order[i].second, order[i].first, megabytes(order[i].first * order[i].second)));
    if (sizes.other_count) out(std::format("  (and {} blocks of other sizes, {})", sizes.other_count, megabytes(sizes.other_bytes)));
    if (mark) {
        heap_mark = std::move(now);
        heap_marked = true;
        out("Marked. Do what you want to measure, then run `perf memory heap` again.");
        return;
    }
    if (!heap_marked) return;
    // What was added since the mark, by size: a leak shows as a size whose count only climbs.
    std::vector<std::tuple<std::uint64_t, std::int64_t>> added; // size, count added
    for (const auto& [size, count] : now) {
        const auto before = heap_mark.find(size);
        const auto was = before == heap_mark.end() ? 0 : before->second;
        if (count > was) added.emplace_back(size, static_cast<std::int64_t>(count - was));
    }
    std::ranges::sort(added, [](const auto& a, const auto& b) {
        return std::get<0>(a) * std::get<1>(a) > std::get<0>(b) * std::get<1>(b);
    });
    out("Added since the mark, by size:");
    for (std::size_t i = 0; i < added.size() && i < most_sizes; ++i)
        out(std::format("  +{} x {} bytes = {}", std::get<1>(added[i]), std::get<0>(added[i]),
            megabytes(std::get<0>(added[i]) * static_cast<std::uint64_t>(std::get<1>(added[i])))));
}

namespace {
struct Gpu {
    std::string name;
    std::uint64_t local{}, local_budget{}, shared{}, shared_budget{};
};
std::vector<Gpu> read_gpus() {
    std::vector<Gpu> gpus;
    IDXGIFactory4* factory{};
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory))) || !factory) return gpus;
    IDXGIAdapter1* adapter{};
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 description{};
        IDXGIAdapter3* adapter3{};
        if (SUCCEEDED(adapter->GetDesc1(&description)) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared);
            // (Only an adapter this process uses.)
            if (local.CurrentUsage || shared.CurrentUsage) {
                Gpu gpu{{}, local.CurrentUsage, local.Budget, shared.CurrentUsage, shared.Budget};
                for (const wchar_t* c = description.Description; *c; ++c) gpu.name += static_cast<char>(*c < 128 ? *c : '?');
                gpus.push_back(std::move(gpu));
            }
            adapter3->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return gpus;
}
} // namespace

void gpu_report(const Line& out) {
    for (const auto& gpu : read_gpus())
        out(std::format("GPU {}: {} of video memory (budget {}), {} of system memory (budget {}).", gpu.name,
            megabytes(gpu.local), megabytes(gpu.local_budget), megabytes(gpu.shared), megabytes(gpu.shared_budget)));
}

std::string owners_line() {
    if (!running.load()) return {};
    std::lock_guard lock(watch_mutex);
    return watched_owners;
}
std::string gpu_line() {
    std::string line;
    for (const auto& gpu : read_gpus())
        line += std::format("{}GPU: {} video memory, {} system memory", line.empty() ? "" : "; ", megabytes(gpu.local), megabytes(gpu.shared));
    return line;
}

std::filesystem::path write_report() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const auto path = logging::status().directory / std::format("memory-{:04}{:02}{:02}-{:02}{:02}{:02}.txt", now.wYear, now.wMonth,
        now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::string text;
    const Line add = [&](const std::string& line) { text += line + "\n"; };
    owners_report(add, 60);
    add("");
    heap_report(add, false, 60);
    if (asking()) {
        add("");
        asked_report(add);
    }
    std::ofstream file(path, std::ios::binary);
    file << text;
    return file ? path : std::filesystem::path{};
}
}
