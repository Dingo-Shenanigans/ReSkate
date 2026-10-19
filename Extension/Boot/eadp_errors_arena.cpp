#include "eadp_errors_arena.h"

#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/20260929/eadp_errors_arena.h"

#include <Windows.h>
#include <array>
#include <cstring>
#include <limits>

namespace dingosdk {
namespace {
namespace contract = addr::eadp_errors_arena;

constexpr std::size_t stub_page_bytes = 0x1000;

bool supported_image(std::uintptr_t base) noexcept {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    return base && memory::read(base, dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
        dos.e_lfanew <= 0x1000 && memory::read(base + dos.e_lfanew, nt) && nt.Signature == IMAGE_NT_SIGNATURE &&
        nt.OptionalHeader.SizeOfImage == supported_build::game_image_size;
}

bool rel32(std::uintptr_t next_instruction, std::uintptr_t target, std::int32_t& displacement) noexcept {
    const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(next_instruction);
    if (delta < std::numeric_limits<std::int32_t>::min() || delta > std::numeric_limits<std::int32_t>::max()) return false;
    displacement = static_cast<std::int32_t>(delta);
    return true;
}

bool write_code(std::uintptr_t address, const void* bytes, std::size_t size) noexcept {
    auto* const destination = reinterpret_cast<void*>(address);
    DWORD previous{};
    if (!VirtualProtect(destination, size, PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(destination, bytes, size);
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), destination, size) != FALSE;
    DWORD discarded{};
    return VirtualProtect(destination, size, previous, &discarded) != FALSE && flushed;
}
} // namespace

bool start_eadp_errors_arena(std::uintptr_t base, std::string& error) {
    std::array<std::uint8_t, contract::size_load.size()> actual{};
    if (!supported_image(base) || !memory::read_bytes(base + contract::size_load_rva, actual.data(), actual.size()) ||
        actual != contract::size_load) {
        error = "the arena's factory does not match the supported game";
        return false;
    }
    auto* const stub = static_cast<std::uint8_t*>(memory::allocate_near_image(base, supported_build::game_image_size, stub_page_bytes));
    if (!stub) {
        error = "no room for its stub near the game";
        return false;
    }
    const auto at = reinterpret_cast<std::uintptr_t>(stub);
    // The two moved instructions, with the size capped between them:
    //   mov eax, [rax+0x7c] / cmp eax, cap / jbe +5 / mov eax, cap / lea rcx, [vtable] / jmp resume
    std::array<std::uint8_t, 27> code{0x8b, 0x40, 0x7c, 0x3d, 0, 0, 0, 0, 0x76, 0x05, 0xb8, 0, 0, 0, 0,
        0x48, 0x8d, 0x0d, 0, 0, 0, 0, 0xe9, 0, 0, 0, 0};
    std::memcpy(code.data() + 4, &eadp_errors_arena_bytes, 4);
    std::memcpy(code.data() + 11, &eadp_errors_arena_bytes, 4);
    std::int32_t vtable{}, resume{}, into{};
    if (!rel32(at + 22, base + contract::vtable_rva, vtable) || !rel32(at + 27, base + contract::resume_rva, resume) ||
        !rel32(base + contract::size_load_rva + 5, at, into)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        error = "its stub is out of reach of the game";
        return false;
    }
    std::memcpy(code.data() + 18, &vtable, 4);
    std::memcpy(code.data() + 23, &resume, 4);
    std::memset(stub, 0xcc, stub_page_bytes);
    std::memcpy(stub, code.data(), code.size());
    DWORD previous{};
    if (!VirtualProtect(stub, stub_page_bytes, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), stub, stub_page_bytes)) {
        VirtualFree(stub, 0, MEM_RELEASE);
        error = "its stub could not be made executable";
        return false;
    }
    std::array<std::uint8_t, contract::size_load.size()> jump{0xe9};
    std::memcpy(jump.data() + 1, &into, 4);
    std::memset(jump.data() + 5, 0x90, jump.size() - 5);
    if (!write_code(base + contract::size_load_rva, jump.data(), jump.size())) {
        (void)write_code(base + contract::size_load_rva, contract::size_load.data(), contract::size_load.size());
        VirtualFree(stub, 0, MEM_RELEASE);
        error = "the size load could not be redirected";
        return false;
    }
    return true;
}
} // namespace dingosdk
