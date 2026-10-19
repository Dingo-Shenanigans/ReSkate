#include "local_crash_note.h"
#include <Windows.h>
#include <cstdio>
#include <string>

namespace dingosdk {
namespace {
LPTOP_LEVEL_EXCEPTION_FILTER previous_filter{};

LONG WINAPI note_unhandled(EXCEPTION_POINTERS* info) {
    std::wstring path(32768, L'\0');
    const auto length = GetEnvironmentVariableW(L"RESKATE_CRASH_NOTE_FILE", path.data(), static_cast<DWORD>(path.size()));
    if (length && length < path.size() && info && info->ExceptionRecord) {
        path.resize(length);
        const auto address = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
        HMODULE module{};
        wchar_t module_path[MAX_PATH]{};
        std::uintptr_t offset = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(info->ExceptionRecord->ExceptionAddress), &module)) {
            GetModuleFileNameW(module, module_path, MAX_PATH);
            offset = address - reinterpret_cast<std::uintptr_t>(module);
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        char text[1024]{};
        std::snprintf(text, sizeof text, "%04d-%02d-%02d %02d:%02d:%02d\r\nexception 0x%08lX at 0x%llX (thread %lu)\r\nmodule %ls\r\noffset 0x%llX\r\n",
                      now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                      static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode), static_cast<unsigned long long>(address),
                      GetCurrentThreadId(), module_path, static_cast<unsigned long long>(offset));
        if (HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, text, static_cast<DWORD>(std::char_traits<char>::length(text)), &written, nullptr);
            CloseHandle(file);
        }
    }
    return previous_filter ? previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void start_local_crash_note() {
    if (!GetEnvironmentVariableW(L"RESKATE_CRASH_NOTE_FILE", nullptr, 0)) return;
    previous_filter = SetUnhandledExceptionFilter(note_unhandled);
}
} // namespace dingosdk
