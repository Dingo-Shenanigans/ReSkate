#include "local_music_safety.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/local_music.h"
#include "Engine/Game/Build/supported_build.h"
#include <Windows.h>
#include <array>
#include <cstring>

namespace dingosdk::profile_runtime {
namespace {
using namespace dingosdk::game::build::v20260929;
using local_music::playlist_lookup_patch_bytes;
using local_music::playlist_lookup_safety_contract;
using local_music::playlist_lookup_safety_rva;

void report(logging::Level level, const char* message) noexcept {
    logging::write(level, logging::Channel::music, message);
}

bool write_patch(std::uintptr_t address, const std::array<unsigned char, 13>& bytes) noexcept {
    DWORD previous{};
    auto* const destination = reinterpret_cast<void*>(address);
    if (!VirtualProtect(destination, bytes.size(), PAGE_EXECUTE_READWRITE, &previous)) return false;
    std::memcpy(destination, bytes.data(), bytes.size());
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), destination, bytes.size()) != FALSE;
    DWORD discarded{};
    const bool restored = VirtualProtect(destination, bytes.size(), previous, &discarded) != FALSE;
    return flushed && restored;
}
}

bool install_playlist_lookup_safety(std::uintptr_t base) noexcept {
    const DWORD saved = GetLastError();
    constexpr auto size = playlist_lookup_safety_contract.size();
    const auto rva = playlist_lookup_safety_rva;
    if (base == 0 || rva >= supported_build::game_image_size ||
        size > supported_build::game_image_size - rva) {
        report(logging::Level::error, "Music playlist lookup safety patch: the site is outside the game image.");
        SetLastError(saved);
        return false;
    }
    const auto address = base + rva;
    std::array<unsigned char, size> actual{};
    if (!memory::read_bytes(address, actual.data(), actual.size())) {
        report(logging::Level::error, "Music playlist lookup safety patch: the site is unreadable.");
        SetLastError(saved);
        return false;
    }
    if (actual == playlist_lookup_patch_bytes) {
        SetLastError(saved); // Already guarded by an earlier install.
        return true;
    }
    if (actual != playlist_lookup_safety_contract) {
        report(logging::Level::error, "Music playlist lookup safety patch: the site does not match the supported build.");
        SetLastError(saved);
        return false;
    }
    if (!write_patch(address, playlist_lookup_patch_bytes)) {
        report(logging::Level::error, "Music playlist lookup safety patch: could not make the site writable.");
        SetLastError(saved);
        return false;
    }
    logging::event(logging::Channel::music, "{\"event\":\"music_playlist_lookup_safety\"}");
    SetLastError(saved);
    return true;
}
}
