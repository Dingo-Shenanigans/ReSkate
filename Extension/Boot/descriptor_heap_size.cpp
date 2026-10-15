#include "descriptor_heap_size.h"

#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/supported_build.h"
#include "Engine/Game/Build/20260929/descriptor_heap_size.h"

#include <Windows.h>
#include <array>
#include <cstring>

namespace dingosdk {
namespace {
namespace contract = addr::descriptor_heap_size;

bool supported_image(std::uintptr_t base) noexcept {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    return base && memory::read(base, dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
        dos.e_lfanew <= 0x1000 && memory::read(base + dos.e_lfanew, nt) && nt.Signature == IMAGE_NT_SIGNATURE &&
        nt.OptionalHeader.SizeOfImage == supported_build::game_image_size;
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

bool start_descriptor_heap_size(std::uintptr_t base, std::string& error) {
    std::array<std::uint8_t, contract::cap_load.size()> actual{};
    std::array<std::uint8_t, contract::total_cap.size()> total{};
    if (!supported_image(base) || !memory::read_bytes(base + contract::cap_load_rva, actual.data(), actual.size()) ||
        actual != contract::cap_load || !memory::read_bytes(base + contract::total_cap_rva, total.data(), total.size()) ||
        total != contract::total_cap) {
        error = "the renderer's descriptor heap setup does not match the supported game";
        return false;
    }
    // The same divide, with the cap a constant instead of the setting (which is never set):
    //   div r9d / mov edi, eax / mov ecx, cap / or eax, -1 / nop
    std::array<std::uint8_t, contract::cap_load.size()> code{
        0x41, 0xf7, 0xf1, 0x8b, 0xf8, 0xb9, 0, 0, 0, 0, 0x83, 0xc8, 0xff, 0x90};
    std::memcpy(code.data() + 6, &frame_descriptor_cap, 4);
    // The total, a constant, then capped at 1,000,000 as before: mov ecx, total / nop (4 bytes).
    std::array<std::uint8_t, contract::total_cap.size()> load{0xb9, 0, 0, 0, 0, 0x0f, 0x1f, 0x40, 0x00};
    std::memcpy(load.data() + 1, &heap_descriptor_total, 4);
    if (!write_code(base + contract::cap_load_rva, code.data(), code.size()) ||
        !write_code(base + contract::total_cap_rva, load.data(), load.size())) {
        (void)write_code(base + contract::cap_load_rva, contract::cap_load.data(), contract::cap_load.size());
        (void)write_code(base + contract::total_cap_rva, contract::total_cap.data(), contract::total_cap.size());
        error = "the heap sizes could not be patched";
        return false;
    }
    return true;
}
} // namespace dingosdk
