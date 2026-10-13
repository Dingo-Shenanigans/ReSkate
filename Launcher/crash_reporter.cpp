#include <Windows.h>
#include "crash_reporter.h"
#include <DbgHelp.h>
#include "Engine/Core/Debug/protocol.h"
#include "Engine/Core/Debug/upload.h"
#include "Engine/Core/Debug/native_dump.h"
#include "Engine/Core/Debug/multipart.h"
#include "Engine/Game/Build/supported_build.h"
#include "crash_report_config.h"
#include <cstdlib>
#include <fstream>
#include <format>
#include <thread>

namespace {
using namespace dingosdk;
using namespace dingosdk::crash_report;
std::string utf8(const wchar_t* text) {
    const auto size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}
bool publish_dump(const std::filesystem::path& temporary, const SharedReport& report, const char* source) {
    auto dump = temporary; dump.replace_extension(L".dmp");
    Json attributes = Json::object();
    attributes["application"] = "ReSkate";
    attributes["process"] = utf8(report.application.data());
    attributes["version"] = build_version;
#ifdef _DEBUG
    attributes["build.configuration"] = "Debug";
#else
    attributes["build.configuration"] = "Release";
#endif
    attributes["game.supported_sha256"] = supported_build::game_sha256;
    attributes["error.type"] = "crash";
    attributes["report.id"] = utf8(dump.stem().c_str());
    attributes["capture.source"] = source;
    const auto log = snapshot_log(std::filesystem::path(report.directory.data()).parent_path() / L"ReSkate.log", dump);
    attributes["attachment.log"] = log == LogSnapshot::complete ? "complete" : log == LogSnapshot::tail ? "tail" : "unavailable";
    std::ofstream metadata(dump.wstring() + L".json", std::ios::binary);
    metadata << attributes.dump();
    metadata.close();
    if (!metadata || !MoveFileExW(temporary.c_str(), dump.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        DeleteFileW((dump.wstring() + L".json").c_str());
        DeleteFileW(log_attachment_path(dump).c_str());
        return false;
    }
    return true;
}
bool save_dump(HANDLE process, const SharedReport& report, const std::filesystem::path& directory) {
    const auto id = std::format(L"{}-{}-{}", GetCurrentProcessId(), GetTickCount64(), report.thread_id);
    const auto temporary = directory / (id + L".tmp");
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_EXCEPTION_INFORMATION exception{report.thread_id, report.exception, TRUE};
    constexpr auto flags = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    const auto written = MiniDumpWriteDump(process, GetProcessId(process), file.value, flags, &exception, nullptr, nullptr);
    const auto durable = written && FlushFileBuffers(file.value);
    CloseHandle(file.release());
    if (!durable) { DeleteFileW(temporary.c_str()); return false; }
    return publish_dump(temporary, report, "unhandled-exception");
}
bool collect_native_dump(HANDLE process, const SharedReport& report, const std::filesystem::path& directory) {
    const auto native = find_native_dump(report.native_directory.data(), GetProcessId(process), process_creation_seconds(process));
    if (!native) return false;
    const auto temporary = directory / std::format(L"native-{}-{}-{}.tmp", native->process_id, native->process_created, native->thread_id);
    if (!CopyFileW(native->path.c_str(), temporary.c_str(), TRUE)) return false;
    return publish_dump(temporary, report, "game-native");
}
int run(int argc, wchar_t** argv) {
    if (argc != handle_count + 2 || std::wstring_view(argv[1]) != L"--reskate-crash-helper") return 2;
    std::array<Handle, handle_count> handles;
    for (std::size_t i = 0; i < handle_count; ++i) {
        const std::wstring_view value(argv[i + 2]);
        if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos) return 2;
        wchar_t* end{};
        const auto number = wcstoull(argv[i + 2], &end, 10);
        if (!number || *end) return 2;
        handles[i].value = reinterpret_cast<HANDLE>(number);
        DWORD flags{};
        if (!GetHandleInformation(handles[i].value, &flags)) return 2;
        SetHandleInformation(handles[i].value, HANDLE_FLAG_INHERIT, 0);
    }
    auto* shared = static_cast<SharedReport*>(MapViewOfFile(handles[mapping].value, FILE_MAP_READ, 0, 0, sizeof(SharedReport)));
    if (!shared) return 3;
    struct View { void* value; ~View() { UnmapViewOfFile(value); } } view{shared};
    if (shared->version != protocol_version || shared->url.back() || shared->directory.back() ||
        shared->application.back() || shared->native_directory.back()) return 3;
    const std::wstring url(shared->url.data());
    Endpoint endpoint;
    if (!parse_endpoint(url, endpoint)) return 3;
    const auto directory = queue_directory(shared->directory.data(), url);
    std::filesystem::create_directories(directory);
    // Uploading an old queue must never delay capturing a new crash.
    std::jthread uploader([&] { upload_pending(url, directory); });
    SetEvent(handles[ready].value);
    const HANDLE wait_for[]{handles[requested].value, handles[stopping].value, handles[parent].value};
    const auto reason = WaitForMultipleObjects(3, wait_for, FALSE, INFINITE);
    bool saved{};
    if (reason == WAIT_OBJECT_0) {
        try { saved = save_dump(handles[parent].value, *shared, directory); } catch (...) {}
        // Release the crashed process before any network activity or uploader join.
        SetEvent(handles[captured].value);
        std::ofstream(directory / L"capture-status.txt") << (saved ? "Captured unhandled exception.\n" : "Could not capture minidump; local crash log retained.\n");
    } else if (reason == WAIT_OBJECT_0 + 2 && shared->native_directory[0]) {
        // The game's native fatal handler may replace/bypass our Windows filter.
        // After exit all native dump writers are closed. Preserve their original
        // exception context rather than taking a context-free termination dump.
        try { saved = collect_native_dump(handles[parent].value, *shared, directory); } catch (...) {}
        DWORD exit_code{}; GetExitCodeProcess(handles[parent].value, &exit_code);
        std::ofstream(directory / L"capture-status.txt") << std::format(
            "Game process {} exited (0x{:08x}); {}\n", GetProcessId(handles[parent].value), exit_code,
            saved ? "queued its native crash dump." : "no native dump matched this process and start time.");
    } else return 0;
    uploader.join();
    if (saved) upload_pending(url, directory);
    return saved ? 0 : 4;
}
}
int dingosdk::crash_report::run_reporter(int argc, wchar_t** argv) noexcept {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try { return run(argc, argv); } catch (...) { return 1; }
}
