// The patch helpers (Engine/Core/Hooks/patches.h) on memory the test owns: a table, a read-only
// page standing in for an import address table, and an executable page whose code is jumped
// over, run, put back and broken into.
#include "Engine/Core/Hooks/patches.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {
int failures = 0;

void expect(bool condition, std::string_view what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
}

int first() { return 1; }
int second() { return 2; }
int third() { return 3; }

DWORD protection(const void* address) {
    MEMORY_BASIC_INFORMATION page{};
    return VirtualQuery(address, &page, sizeof(page)) ? page.Protect : 0;
}

bool active(std::uintptr_t address) {
    for (const auto& record : dingosdk::patch_records())
        if (record.address == address) return record.active;
    return false;
}

void table_slot() {
    using namespace dingosdk;
    void* table[2]{reinterpret_cast<void*>(&first), reinterpret_cast<void*>(&second)};
    void* original{};
    expect(patch_table_slot("test row", &table[1], reinterpret_cast<void*>(&first), reinterpret_cast<void*>(&third),
               &original) == HookBytesDiffer, "a table slot not holding the expected original is refused");
    expect(table[1] == reinterpret_cast<void*>(&second) && !original, "a refused table slot is left as it was");
    expect(patch_table_slot("test row", &table[1], reinterpret_cast<void*>(&second), reinterpret_cast<void*>(&third),
               &original) == HookOk, "a table slot holding the expected original is swapped");
    expect(table[1] == reinterpret_cast<void*>(&third) && original == reinterpret_cast<void*>(&second),
        "the swapped table slot holds the replacement and the original is returned");
    expect(table[0] == reinterpret_cast<void*>(&first), "the next slot is untouched");
    expect(active(reinterpret_cast<std::uintptr_t>(&table[1])), "the table slot is listed as patched");
    expect(patch_slot_restore(&table[1]) == HookOk && table[1] == reinterpret_cast<void*>(&second),
        "the table slot is put back");
    expect(!active(reinterpret_cast<std::uintptr_t>(&table[1])), "the restored table slot is listed as not patched");
    expect(patch_slot_restore(&table[1]) == HookNotFound, "a slot not patched is not restored again");
}

void import_slot() {
    using namespace dingosdk;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto* page = static_cast<void**>(VirtualAlloc(nullptr, system.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    expect(page != nullptr, "an import page is allocated");
    if (!page) return;
    page[3] = reinterpret_cast<void*>(&first);
    DWORD previous{};
    VirtualProtect(page, system.dwPageSize, PAGE_READONLY, &previous);
    void* original{};
    expect(patch_import_slot("test import", &page[3], reinterpret_cast<void*>(&first), reinterpret_cast<void*>(&second),
               &original) == HookOk, "a read-only import slot is swapped");
    expect(page[3] == reinterpret_cast<void*>(&second) && original == reinterpret_cast<void*>(&first),
        "the import slot holds the replacement");
    expect(protection(page) == PAGE_READONLY, "the import page is read-only again");
    // A slot something else has since rebound is not put back over it.
    VirtualProtect(page, system.dwPageSize, PAGE_READWRITE, &previous);
    page[3] = reinterpret_cast<void*>(&third);
    VirtualProtect(page, system.dwPageSize, PAGE_READONLY, &previous);
    expect(patch_slot_restore(&page[3]) == HookBytesDiffer && page[3] == reinterpret_cast<void*>(&third),
        "an import slot rebound by someone else is left alone");
    VirtualFree(page, 0, MEM_RELEASE);
}

void code_jump_encoding() {
    using namespace dingosdk;
    std::array<std::uint8_t, 7> bytes{};
    expect(code_jump_bytes(0x1000, 0x2000, bytes), "a near jump is encoded");
    expect(bytes == std::array<std::uint8_t, 7>{0xe9, 0xfb, 0x0f, 0x00, 0x00, 0x90, 0x90},
        "the jump is E9 rel32 from the next instruction, then NOPs");
    expect(code_jump_bytes(0x2000, 0x1000, bytes) &&
               bytes == std::array<std::uint8_t, 7>{0xe9, 0xfb, 0xef, 0xff, 0xff, 0x90, 0x90},
        "a backward jump has a negative displacement");
    std::array<std::uint8_t, 4> short_bytes{};
    expect(!code_jump_bytes(0x1000, 0x2000, short_bytes), "four bytes cannot hold a jump");
    expect(!code_jump_bytes(0x10000000000, 0x10000000000 + 0x100000000, bytes), "a jump beyond 2 GB is refused");
}

// mov eax, imm32 / ret, with NOPs after: ten bytes, room for a five-byte jump and its padding.
constexpr std::array<std::uint8_t, 10> returns_seven{0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3, 0x90, 0x90, 0x90, 0x90};
constexpr std::array<std::uint8_t, 6> returns_forty_two{0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
std::uintptr_t breakpoint_site{};
int breakpoints_taken{};

LONG CALLBACK take_breakpoint(EXCEPTION_POINTERS* pointers) {
    if (pointers->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT ||
        reinterpret_cast<std::uintptr_t>(pointers->ExceptionRecord->ExceptionAddress) != breakpoint_site)
        return EXCEPTION_CONTINUE_SEARCH;
    ++breakpoints_taken;
    if (dingosdk::patch_breakpoint_restore(breakpoint_site, returns_seven[0]) != dingosdk::HookOk)
        return EXCEPTION_CONTINUE_SEARCH;
    pointers->ContextRecord->Rip = breakpoint_site;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void code() {
    using namespace dingosdk;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, system.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    expect(page != nullptr, "a code page is allocated");
    if (!page) return;
    std::memset(page, 0xcc, system.dwPageSize);
    std::memcpy(page, returns_seven.data(), returns_seven.size());
    std::memcpy(page + 0x800, returns_forty_two.data(), returns_forty_two.size());
    DWORD previous{};
    VirtualProtect(page, system.dwPageSize, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), page, system.dwPageSize);
    const auto at = reinterpret_cast<std::uintptr_t>(page);
    const auto run = reinterpret_cast<int (*)()>(page);
    expect(run() == 7, "the code runs as written");

    std::array<std::uint8_t, 10> other = returns_seven;
    other[1] = 0x08;
    expect(patch_code_jump("test jump", at, other, at + 0x800) == HookBytesDiffer, "a jump over other bytes is refused");
    expect(std::memcmp(page, returns_seven.data(), returns_seven.size()) == 0, "refused code is left as it was");
    expect(patch_code_jump("test jump", at, returns_seven, at + 0x100000000) == HookOutOfReach,
        "a jump out of reach is refused");

    expect(patch_code_jump("test jump", at, returns_seven, at + 0x800) == HookOk, "a jump is written over the code");
    const std::array<std::uint8_t, 10> jump{0xe9, 0xfb, 0x07, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90};
    expect(std::memcmp(page, jump.data(), jump.size()) == 0, "the code holds jmp +0x7fb and five NOPs");
    expect(protection(page) == PAGE_EXECUTE_READ, "the code page is execute-read again");
    expect(active(at), "the jump is listed as patched");
    expect(run() == 42, "the code now runs at the jump's destination");
    expect(patch_code_restore(at, returns_seven) == HookOk && run() == 7, "the code is put back and runs as written");
    expect(!active(at), "the restored code is listed as not patched");

    const std::array<std::uint8_t, 1> nine{0x09};
    expect(patch_code("test byte", at + 1, std::span{returns_seven}.subspan(1, 1), nine) == HookOk && run() == 9,
        "a byte is replaced once it holds the expected one");
    expect(patch_code("test byte", at + 1, std::span{returns_seven}.subspan(1, 1), nine) == HookBytesDiffer,
        "a byte already replaced is not replaced again");
    expect(patch_code_restore(at + 1, std::span{returns_seven}.subspan(1, 1)) == HookOk && run() == 7, "the byte is put back");

    breakpoint_site = at;
    auto* const handler = patch_breakpoint_handler("test breakpoints", take_breakpoint);
    expect(handler != nullptr, "a breakpoint handler is added");
    expect(patch_breakpoint("test breakpoint", at, 0x90) == HookBytesDiffer, "a breakpoint over another opcode is refused");
    expect(patch_breakpoint("test breakpoint", at, returns_seven[0]) == HookOk && page[0] == 0xcc,
        "an int3 is written over the opcode");
    expect(run() == 7 && breakpoints_taken == 1, "the breakpoint is taken, its opcode put back, and the code goes on");
    expect(page[0] == returns_seven[0] && !active(at), "the opcode is back after the breakpoint");
    patch_breakpoint_handler_remove(handler);
    expect(!active(reinterpret_cast<std::uintptr_t>(handler)), "the handler is listed as removed");
    VirtualFree(page, 0, MEM_RELEASE);
}
} // namespace

int main() {
    table_slot();
    import_slot();
    code_jump_encoding();
    code();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("patch helpers: all passed\n");
    return 0;
}
