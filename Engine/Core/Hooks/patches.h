#pragma once
#include "hooks.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The patches Detours does not make: a function pointer swapped in one of the game's tables, an
// import address table slot, and bytes, a jump or a breakpoint written into the game's code. Each
// helper refuses (HookBytesDiffer, nothing written) unless the place holds what the caller expects,
// writes with the same steps the features used before they moved here, and is listed by
// patch_records(). None needs hook_initialize: the startup breakpoints run before it.
// Names are kept, not copied: pass string literals.
namespace dingosdk {
// A pointer slot in a table the game reads (a script-native row, a binding record): made writable,
// stored, its protection put back. original, when given, receives expected.
HookStatus patch_table_slot(const char* name, void** slot, void* expected, void* replacement,
    void** original = nullptr) noexcept;
// The same for an import address table slot, stored with InterlockedExchangePointer.
HookStatus patch_import_slot(const char* name, void** slot, void* expected, void* replacement,
    void** original = nullptr) noexcept;
// Puts back the pointer a patch_*_slot replaced, if the slot still holds the replacement.
HookStatus patch_slot_restore(void** slot) noexcept;

// out filled with a jump from at to destination: E9 and a rel32 displacement from at + 5, then
// NOPs (0x90) to the end. False when out is shorter than 5 bytes or the destination is out of reach.
bool code_jump_bytes(std::uintptr_t at, std::uintptr_t destination, std::span<std::uint8_t> out) noexcept;
// replacement written over the code at address once it holds expected (the same length):
// VirtualProtect to PAGE_EXECUTE_READWRITE, copy, FlushInstructionCache, the old protection back.
HookStatus patch_code(const char* name, std::uintptr_t address, std::span<const std::uint8_t> expected,
    std::span<const std::uint8_t> replacement) noexcept;
// A jump to destination over all expected.size() bytes at address (code_jump_bytes, patch_code).
HookStatus patch_code_jump(const char* name, std::uintptr_t address, std::span<const std::uint8_t> expected,
    std::uintptr_t destination) noexcept;
// original written back over a code patch the same way, without checking what is there: the undo
// the features do after a failed or abandoned install, and a breakpoint's restore in its handler.
HookStatus patch_code_restore(std::uintptr_t address, std::span<const std::uint8_t> original) noexcept;

// An int3 (0xCC) over the instruction whose first byte at address is opcode, and its undo. The
// handler that takes the breakpoint is the feature's, added first with patch_breakpoint_handler
// (AddVectoredExceptionHandler, first in the chain). Restoring takes no allocation, so a handler
// can do it.
HookStatus patch_breakpoint(const char* name, std::uintptr_t address, std::uint8_t opcode) noexcept;
HookStatus patch_breakpoint_restore(std::uintptr_t address, std::uint8_t opcode) noexcept;
void* patch_breakpoint_handler(const char* name, PVECTORED_EXCEPTION_HANDLER handler) noexcept;
void patch_breakpoint_handler_remove(void* handle) noexcept;

// What the helpers above have written, one entry per place, in the order first patched.
struct PatchRecord {
    const char* kind{};
    const char* name{};
    std::uintptr_t address{};
    std::size_t size{};
    bool active{};
};
std::vector<PatchRecord> patch_records();
}
