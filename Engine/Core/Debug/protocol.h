#pragma once
#include <Windows.h>
#include <array>
#include <string>

namespace dingosdk::crash_report {
inline constexpr DWORD protocol_version = 3;
inline constexpr DWORD capture_timeout_ms = 15000;
struct SharedReport {
    DWORD version{protocol_version};
    DWORD thread_id{};
    EXCEPTION_POINTERS* exception{}; // Address in the parent; MiniDumpWriteDump uses ClientPointers=TRUE.
    std::array<wchar_t, 4096> url{};
    std::array<wchar_t, 32768> directory{};
    std::array<wchar_t, 260> application{};
    std::array<wchar_t, 32768> native_directory{};
    // What the game was doing, for the report's tags: the level it loaded last, and the graphics
    // card it renders on with its driver's version. Written by the game as they change, read by
    // the helper after a crash (a torn read only spoils a tag).
    std::array<wchar_t, 512> destination{};
    std::array<wchar_t, 128> gpu_name{};
    std::array<wchar_t, 32> gpu_driver{};
};
// Explicitly inherited unnamed handles; no token or paths on the command line.
enum HandleIndex : std::size_t { mapping, requested, captured, stopping, ready, parent, handle_count };
struct Handle {
    HANDLE value{};
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE release() { const auto result = value; value = nullptr; return result; }
};
inline std::wstring environment(const wchar_t* name) {
    std::array<wchar_t, 32768> buffer{};
    const auto size = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    return size && size < buffer.size() ? std::wstring(buffer.data(), size) : std::wstring{};
}
}
