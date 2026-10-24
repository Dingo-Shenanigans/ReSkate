#pragma once
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>

// One game at a time in a folder. A second Skate.exe started over the same folder merges the mods
// again into the Mods\.reskate patch the first one has open, and the first then crashed reading
// it (Skate.exe+0x41e0f81 with "Cannot place ...cas_02.cas" and "Save is already in use"; two
// launchers started within the same millisecond in one log; 2026-10-21). A running game's
// ReSkate.dll holds a named mutex for its folder (Runtime/runtime_init.cpp) and the launcher
// will not merge or start a game while it is held (Launcher/gui_launcher.cpp, main.cpp).
namespace dingosdk::game_instance {
// The mutex's name for a game folder: the folder's path, ignoring case, hashed.
inline std::wstring mutex_name(const std::filesystem::path& folder) {
    auto text = folder.lexically_normal().wstring();
    while (!text.empty() && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto c : text) {
        hash ^= static_cast<std::uint16_t>(c);
        hash *= 0x100000001b3ull;
    }
    return std::format(L"Local\\ReSkate.Game.{:016x}", hash);
}
// Whether a game holds the folder's mutex now.
inline bool running(const std::filesystem::path& folder) {
    const auto handle = OpenMutexW(SYNCHRONIZE, FALSE, mutex_name(folder).c_str());
    if (!handle) return false;
    CloseHandle(handle);
    return true;
}
// Takes the folder's mutex for the rest of this process. False when another process holds it.
inline bool claim(const std::filesystem::path& folder) {
    const auto handle = CreateMutexW(nullptr, FALSE, mutex_name(folder).c_str());
    if (!handle) return true; // (no mutex to be had: nothing is refused for it)
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        return false;
    }
    return true; // (kept open until the process ends)
}
}
