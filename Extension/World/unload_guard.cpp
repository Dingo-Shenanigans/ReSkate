#include "unload_guard.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/unload_guard.h"
#include "Engine/Game/Build/supported_build.h"
#include <Windows.h>
#include <array>
#include <cstring>

namespace dingosdk {
bool start_unload_guard(std::uintptr_t base, std::string& error) noexcept {
    namespace guard = game::build::v20260929::unload_guard;
    const DWORD saved = GetLastError();
    const auto fail = [&](const char* why) {
        try { error = why; } catch (...) {}
        SetLastError(saved);
        return false;
    };
    if (!base || guard::rva >= supported_build::game_image_size ||
        guard::contract.size() > supported_build::game_image_size - guard::rva)
        return fail("the site is outside the game image");
    const auto address = base + guard::rva;
    std::array<unsigned char, guard::contract.size()> actual{};
    if (!memory::read_bytes(address, actual.data(), actual.size())) return fail("the site is unreadable");
    if (actual == guard::patch) {
        SetLastError(saved); // already guarded
        return true;
    }
    if (actual != guard::contract) return fail("the site does not match the supported build");
    DWORD previous{}, discarded{};
    auto* const destination = reinterpret_cast<void*>(address);
    if (!VirtualProtect(destination, guard::patch.size(), PAGE_EXECUTE_READWRITE, &previous))
        return fail("the site could not be made writable");
    std::memcpy(destination, guard::patch.data(), guard::patch.size());
    FlushInstructionCache(GetCurrentProcess(), destination, guard::patch.size());
    VirtualProtect(destination, guard::patch.size(), previous, &discarded);
    SetLastError(saved);
    return true;
}
}
