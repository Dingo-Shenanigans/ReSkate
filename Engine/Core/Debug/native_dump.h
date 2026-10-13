#pragma once
#include <Windows.h>
#include <filesystem>
#include <optional>

namespace dingosdk::crash_report {
struct NativeDump {
    std::filesystem::path path;
    DWORD process_id{}, process_created{}, thread_id{}, exception_code{}, timestamp{};
    ULONG64 exception_address{};
};
// Match the exact process lifetime, not filenames or modification times. The game
// can emit several dumps for one fault; choose the first, preferring its rich mdmp.
std::optional<NativeDump> find_native_dump(const std::filesystem::path& directory,
    DWORD process_id, DWORD process_created) noexcept;
DWORD process_creation_seconds(HANDLE process) noexcept;
}
