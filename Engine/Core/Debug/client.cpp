#include "crash_report.h"
#include "protocol.h"
#include "crash_report_config.h"
#include <algorithm>
#include <format>
#include <vector>

namespace dingosdk::crash_report {
namespace {
std::array<HANDLE, handle_count> handles{};
HANDLE helper{};
SharedReport* report{};
// 0 = ready, 1 = capture owns the IPC, 2 = stopped/initializing.
volatile LONG capturing{2};
struct View {
    void* value{};
    ~View() { if (value) UnmapViewOfFile(value); }
};
template<std::size_t N> bool copy(std::array<wchar_t, N>& target, const std::wstring& value) {
    if (value.size() >= N) return false;
    std::copy(value.begin(), value.end(), target.begin());
    target[value.size()] = 0;
    return true;
}
template<std::size_t N> void copy_cut(std::array<wchar_t, N>& target, std::wstring_view value) noexcept {
    const auto size = std::min(value.size(), N - 1);
    std::copy_n(value.begin(), size, target.begin());
    target[size] = 0;
}
}
StartResult start(const std::filesystem::path& log_directory) noexcept {
    try {
        if (report) return StartResult::ready;
        if (environment(L"RESKATE_CRASH_REPORTING") == L"0") return StartResult::disabled;
        auto url = environment(L"RESKATE_CRASH_REPORT_URL");
        if (url.empty()) url = configured_url;
        if (url.empty()) return StartResult::disabled;
        if (!url.starts_with(L"https://") || url.size() >= SharedReport{}.url.size()) return StartResult::failed;
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&handles), &module)) return StartResult::failed;
        std::array<wchar_t, 32768> module_path{}, process_path{};
        const auto module_size = GetModuleFileNameW(module, module_path.data(), static_cast<DWORD>(module_path.size()));
        const auto process_size = GetModuleFileNameW(nullptr, process_path.data(), static_cast<DWORD>(process_path.size()));
        if (!module_size || module_size >= module_path.size() || !process_size || process_size >= process_path.size())
            return StartResult::failed;
        const auto executable = std::filesystem::path(module_path.data()).parent_path() / L"ReSkateLauncher.exe";
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        std::array<Handle, handle_count> owned;
        owned[mapping].value = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, sizeof(SharedReport), nullptr);
        for (std::size_t i = requested; i <= ready; ++i) owned[i].value = CreateEventW(&security, TRUE, FALSE, nullptr);
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &owned[parent].value,
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, TRUE, 0)) return StartResult::failed;
        std::array<HANDLE, handle_count> inherited{};
        for (std::size_t i = 0; i < handle_count; ++i) {
            if (!owned[i].value) return StartResult::failed;
            inherited[i] = owned[i].value;
        }
        View view{MapViewOfFile(owned[mapping].value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedReport))};
        if (!view.value) return StartResult::failed;
        auto* shared = new (view.value) SharedReport{};
        if (!copy(shared->url, url) || !copy(shared->directory, std::filesystem::absolute(log_directory / L"crashes").wstring()) ||
                !copy(shared->application, std::filesystem::path(process_path.data()).filename().wstring())) return StartResult::failed;
        // Frostbite handles some fatal exceptions itself and writes a native dump
        // before exiting, so those faults never reach our last-chance filter. The
        // game's LocalAppData is redirected to ReSkate\Game (Extension/Boot/user_data_redirect.cpp).
        if (_wcsicmp(shared->application.data(), L"Skate.exe") == 0) {
            const auto local = environment(L"LOCALAPPDATA");
            if (!local.empty() && !copy(shared->native_directory,
                    (std::filesystem::path(local) / L"ReSkate" / L"Game" / L"Skate" / L"CrashDumps").wstring()))
                return StartResult::failed;
        }

        SIZE_T size{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> storage(size);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return StartResult::failed;
        struct Attributes { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Attributes() { DeleteProcThreadAttributeList(value); } } cleanup{attributes};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited.data(), sizeof(inherited), nullptr, nullptr)) return StartResult::failed;
        auto command = std::format(L"\"{}\" --reskate-crash-helper", executable.wstring());
        for (const auto handle : inherited) command += std::format(L" {}", reinterpret_cast<ULONG_PTR>(handle));
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, executable.parent_path().c_str(),
                &startup.StartupInfo, &process)) return StartResult::failed;
        Handle child(process.hProcess), thread(process.hThread);
        // Prevent later child processes from inheriting our IPC and parent handles.
        for (const auto handle : inherited) SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
        const HANDLE wait_for[]{owned[ready].value, child.value};
        if (WaitForMultipleObjects(2, wait_for, FALSE, 5000) != WAIT_OBJECT_0) {
            SetEvent(owned[stopping].value);
            return StartResult::failed;
        }
        for (std::size_t i = 0; i < handle_count; ++i) handles[i] = owned[i].release();
        helper = child.release();
        report = shared;
        view.value = nullptr;
        InterlockedExchange(&capturing, 0);
        return StartResult::ready;
    } catch (...) { return StartResult::failed; }
}
namespace {
void capture_on(DWORD thread_id, EXCEPTION_POINTERS* exception, CaptureSource source) noexcept {
    if (!exception || !thread_id || InterlockedCompareExchange(&capturing, 1, 0) != 0) return;
    report->thread_id = thread_id;
    report->exception = exception;
    report->source = source;
    MemoryBarrier();
    if (SetEvent(handles[requested])) {
        const HANDLE wait_for[]{handles[captured], helper};
        WaitForMultipleObjects(2, wait_for, FALSE, capture_timeout_ms);
    }
}
}
void capture(EXCEPTION_POINTERS* exception) noexcept {
    capture_on(GetCurrentThreadId(), exception, CaptureSource::unhandled_exception);
}
void capture_game_fatal(DWORD thread_id, EXCEPTION_POINTERS* exception) noexcept {
    capture_on(thread_id, exception, CaptureSource::game_fatal_handler);
}
bool capture_stuck_load(std::wstring_view stage, DWORD seconds) noexcept {
    if (!report || InterlockedCompareExchange(&capturing, 1, 0) != 0) return false;
    // The "exception" is this thread where it stands: the helper reads it through the pointers
    // (ClientPointers), so they stay on this stack until it answers.
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = stuck_load_exception;
    record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
    EXCEPTION_POINTERS pointers{&record, &context};
    copy_cut(report->stuck_stage, stage);
    report->stuck_seconds = seconds;
    report->thread_id = GetCurrentThreadId();
    report->exception = &pointers;
    report->source = CaptureSource::stuck_load;
    MemoryBarrier();
    bool answered = false;
    if (SetEvent(handles[requested])) {
        const HANDLE wait_for[]{handles[captured], helper};
        answered = WaitForMultipleObjects(2, wait_for, FALSE, stuck_capture_timeout_ms) == WAIT_OBJECT_0;
    }
    // The helper has put `requested` down again before it raised `captured`. One that did not
    // answer (gone, or still writing) keeps the channel: a crash then goes without a report
    // rather than racing a dump in hand.
    if (!answered) return false;
    ResetEvent(handles[captured]);
    report->exception = nullptr;
    InterlockedExchange(&capturing, 0);
    return true;
}
void set_destination(std::wstring_view destination) noexcept {
    if (auto* const shared = report) copy_cut(shared->destination, destination);
}
void set_gpu(std::wstring_view name, std::wstring_view driver) noexcept {
    if (auto* const shared = report) {
        copy_cut(shared->gpu_name, name);
        copy_cut(shared->gpu_driver, driver);
    }
}
void set_gpu_breadcrumbs(bool on) noexcept {
    if (auto* const shared = report) shared->gpu_breadcrumbs = on ? 1 : 0;
}
void stop() noexcept {
    // An exception already using these handles owns them until process exit.
    // Never unmap its report or close its events from a concurrent shutdown.
    if (InterlockedCompareExchange(&capturing, 2, 0) != 0) return;
    SetEvent(handles[stopping]);
    UnmapViewOfFile(report);
    report = nullptr;
    for (auto& handle : handles) { CloseHandle(handle); handle = nullptr; }
    CloseHandle(helper);
    helper = nullptr;
}
}
