#include "logging_internal.h"
#include "Engine/Core/Debug/crash_report.h"
#include <array>

namespace dingosdk::logging::detail {
namespace {
std::array<wchar_t, 32768> report_path{};
LPTOP_LEVEL_EXCEPTION_FILTER previous{};
volatile LONG reporting{};
bool installed{};
// Last-chance reporting deliberately avoids the logger, heap, CRT formatting,
// symbol loader and their locks. Leave Windows/game exception handling intact.
LONG WINAPI unhandled(EXCEPTION_POINTERS* exception) {
    if (InterlockedCompareExchange(&reporting, 1, 0) == 0 && exception && exception->ExceptionRecord && exception->ContextRecord) {
        char text[2048]{};
        std::size_t used = 0;
        const auto append = [&](const char* value) { while (*value && used + 1 < sizeof(text)) text[used++] = *value++; };
        const auto hex = [&](const char* label, unsigned long long value) {
            append(label); append("=0x");
            constexpr char digits[] = "0123456789abcdef";
            for (int shift = 60; shift >= 0; shift -= 4) if (used + 1 < sizeof(text)) text[used++] = digits[(value >> shift) & 15];
            append("\r\n");
        };
        append("\r\n[CRITICAL] ReSkate unhandled exception\r\n");
        hex("process", GetCurrentProcessId()); hex("thread", GetCurrentThreadId());
        hex("exception", exception->ExceptionRecord->ExceptionCode);
        hex("address", reinterpret_cast<ULONG_PTR>(exception->ExceptionRecord->ExceptionAddress));
        hex("rip", exception->ContextRecord->Rip); hex("rsp", exception->ContextRecord->Rsp);
        hex("rbp", exception->ContextRecord->Rbp);
        if (exception->ExceptionRecord->NumberParameters >= 2 && exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            hex("access_type", exception->ExceptionRecord->ExceptionInformation[0]);
            hex("access_address", exception->ExceptionRecord->ExceptionInformation[1]);
        }
        const auto file = CreateFileW(report_path.data(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written{}; WriteFile(file, text, static_cast<DWORD>(used), &written, nullptr);
            FlushFileBuffers(file); CloseHandle(file);
        }
    }
    crash_report::capture(exception);
    return previous ? previous(exception) : EXCEPTION_CONTINUE_SEARCH;
}
}
void install_crash_report(const std::filesystem::path& directory) noexcept {
    try {
        if (installed) return;
        const auto path = (directory / L"ReSkate.log").wstring();
        if (path.size() >= report_path.size()) return;
        std::copy(path.begin(), path.end(), report_path.begin()); report_path[path.size()] = 0;
        reporting = 0;
        previous = SetUnhandledExceptionFilter(unhandled); installed = true;
    } catch (...) {}
}
void remove_crash_report() noexcept {
    if (!installed) return;
    const auto current = SetUnhandledExceptionFilter(previous);
    if (current != unhandled) SetUnhandledExceptionFilter(current);
    installed = false;
    crash_report::stop();
}
}
