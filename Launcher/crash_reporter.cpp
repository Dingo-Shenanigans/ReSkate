#include <Windows.h>
#include "crash_reporter.h"
#include <DbgHelp.h>
#include "Engine/Core/Debug/protocol.h"
#include "Engine/Core/Debug/upload.h"
#include "Engine/Core/Debug/native_dump.h"
#include "Engine/Core/Debug/report_envelope.h"
#include "Engine/Core/Debug/report_details.h"
#include "Engine/Vfs/mod_list.h"
#include "Engine/Game/Build/supported_build.h"
#include "crash_report_config.h"
#include "reskate_version.h"
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

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
template<std::size_t N> std::string field(const std::array<wchar_t, N>& value) {
    return utf8(std::wstring(value.data(), wcsnlen(value.data(), N)).c_str());
}
// "Levels/Game/DingoLevel_Root/DingoLevel_Root + levels/game/x/x @ Start" -> "x": the sublevel when
// there is one, else the level, by its last name.
std::string map_name(std::string destination) {
    if (const auto at = destination.find(" @ "); at != std::string::npos) destination.resize(at);
    if (const auto plus = destination.find(" + "); plus != std::string::npos) destination.erase(0, plus + 3);
    if (const auto slash = destination.find_last_of('/'); slash != std::string::npos) destination.erase(0, slash + 1);
    return destination;
}
// The folder the crashed game ran from (its Mods folder beside it), or empty.
std::filesystem::path game_folder(HANDLE process) {
    std::array<wchar_t, 32768> image{};
    DWORD length = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process, 0, image.data(), &length)) return {};
    return std::filesystem::path(std::wstring(image.data(), length)).parent_path();
}
// The event fields a report carries besides its tags (report_details.h): the log's last lines,
// the mods the game ran with, an anonymous ID for this install and how long the game had run.
Json event_details(HANDLE process, const SharedReport& report, const std::filesystem::path& dump, std::size_t& mod_count) {
    Json details = Json::object();
    if (auto crumbs = log_breadcrumbs(log_attachment_path(dump)); crumbs.is_object()) details["breadcrumbs"] = std::move(crumbs);
    Json context = Json::object();
    if (const auto root = game_folder(process); !root.empty()) {
        const auto list = mods::scan_mods(root);
        Json enabled = Json::array(), outdated = Json::array(), excluded = Json::array();
        for (const auto& entry : list.entries) {
            if (!entry.enabled) continue;
            const auto name = entry.mod.version.empty() ? entry.mod.name : entry.mod.name + " " + entry.mod.version;
            if (!entry.mod.outdated.empty()) outdated.push_back(name);
            else if (list.excluded.contains(entry.mod.name)) excluded.push_back(name);
            else enabled.push_back(name);
        }
        mod_count = enabled.size();
        context["mods"] = std::move(enabled);
        if (outdated.size()) context["mods_outdated"] = std::move(outdated);
        if (excluded.size()) context["mods_excluded"] = std::move(excluded);
        if (!list.issue.empty()) context["mods_issue"] = list.issue;
    }
    if (const auto started = process_creation_seconds(process); started) {
        const auto now = static_cast<std::int64_t>(std::time(nullptr));
        if (now >= started) context["uptime_seconds"] = now - static_cast<std::int64_t>(started);
    }
    context["type"] = "default";
    Json contexts = Json::object();
    contexts["reskate"] = std::move(context);
    details["contexts"] = std::move(contexts);
    if (const auto id = install_id(std::filesystem::path(report.directory.data())); !id.empty()) {
        Json user = Json::object();
        user["id"] = id;
        details["user"] = std::move(user);
    }
    return details;
}
bool publish_dump(HANDLE process, const std::filesystem::path& temporary, const SharedReport& report, const char* source) {
    auto dump = temporary; dump.replace_extension(L".dmp");
    Json attributes = Json::object();
    attributes["application"] = "ReSkate";
    attributes["process"] = utf8(report.application.data());
    attributes["version"] = build_version;
    // A modified build says so, so ReSkate's maintainers can tell its crashes from their own.
    if (reskate_modified_build) {
        attributes["build.modified"] = true;
        attributes["build.mod"] = std::string(reskate_mod_id);
        attributes["build.mod_version"] = std::string(reskate_mod_version);
    }
#ifdef _DEBUG
    attributes["build.configuration"] = "Debug";
#else
    attributes["build.configuration"] = "Release";
#endif
    attributes["game.supported_sha256"] = supported_build::game_sha256;
    const bool stuck = report.source == CaptureSource::stuck_load;
    attributes["error.type"] = stuck ? "stall" : "crash";
    if (stuck) {
        attributes["load.stage"] = field(report.stuck_stage);
        attributes["load.seconds"] = std::to_string(report.stuck_seconds);
    }
    attributes["report.id"] = utf8(dump.stem().c_str());
    attributes["capture.source"] = source;
    // (Tags for grouping crashes by the level and the graphics card; absent when unknown.)
    if (const auto destination = field(report.destination); !destination.empty()) {
        attributes["game.destination"] = destination;
        attributes["game.map"] = map_name(destination);
    }
    if (const auto gpu = field(report.gpu_name); !gpu.empty()) attributes["gpu.name"] = gpu;
    if (const auto driver = field(report.gpu_driver); !driver.empty()) attributes["gpu.driver"] = driver;
    attributes["gpu.breadcrumbs"] = report.gpu_breadcrumbs ? "on" : "off";
    // The whole PC's memory as the game went: its out-of-memory exits (E_OUTOFMEMORY) come when
    // RAM and page file are full, so a report says how full (in GB, a tag to sort by).
    if (MEMORYSTATUSEX status{sizeof(status)}; GlobalMemoryStatusEx(&status)) {
        constexpr double gigabyte = 1024.0 * 1024.0 * 1024.0;
        attributes["pc.ram_gb"] = std::format("{:.0f}", static_cast<double>(status.ullTotalPhys) / gigabyte);
        attributes["pc.commit_free_gb"] = std::format("{:.1f}", static_cast<double>(status.ullAvailPageFile) / gigabyte);
    }
    const auto log = snapshot_log(std::filesystem::path(report.directory.data()).parent_path() / L"ReSkate.log", dump);
    attributes["attachment.log"] = log == LogSnapshot::complete ? "complete" : log == LogSnapshot::tail ? "tail" : "unavailable";
    // Where it crashed ("Skate.exe+0x3d9ed9d"): Sentry names game code by the nearest export, so a
    // tag to search and sort by. (A stuck load did not crash: its exception is ReSkate's own.)
    if (const auto site = stuck ? std::string{} : crash_site(temporary); !site.empty()) attributes["crash.site"] = site;
    // (Best effort: a report goes without its details rather than not at all.)
    try {
        std::size_t mod_count{};
        auto details = event_details(process, report, dump, mod_count);
        // A stuck load is a warning, one issue for each stage and map: its exception is the same
        // ReSkate thread every time, which Sentry would otherwise group them all by.
        if (stuck) {
            details["level"] = "warning";
            Json fingerprint = Json::array();
            fingerprint.push_back("stuck-load");
            fingerprint.push_back(field(report.stuck_stage));
            fingerprint.push_back(attributes.value("game.map", "no map"));
            details["fingerprint"] = std::move(fingerprint);
        }
        attributes["mods.count"] = std::to_string(mod_count);
        std::ofstream(event_details_path(dump), std::ios::binary) << details.dump();
    } catch (...) { DeleteFileW(event_details_path(dump).c_str()); }
    std::ofstream metadata(dump.wstring() + L".json", std::ios::binary);
    metadata << attributes.dump();
    metadata.close();
    if (!metadata || !MoveFileExW(temporary.c_str(), dump.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        DeleteFileW((dump.wstring() + L".json").c_str());
        DeleteFileW(log_attachment_path(dump).c_str());
        DeleteFileW(event_details_path(dump).c_str());
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
    // With the memory the stacks point at, so a crash on a freed or overwritten object shows that
    // object and the list it was found in, not just the registers (a few MB more a dump).
    constexpr auto flags = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
        MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
    const auto written = MiniDumpWriteDump(process, GetProcessId(process), file.value, flags, &exception, nullptr, nullptr);
    const auto durable = written && FlushFileBuffers(file.value);
    CloseHandle(file.release());
    if (!durable) { DeleteFileW(temporary.c_str()); return false; }
    return publish_dump(process, temporary, report,
        report.source == CaptureSource::game_fatal_handler ? "game-fatal-handler" :
        report.source == CaptureSource::stuck_load ? "stuck-load" : "unhandled-exception");
}
bool collect_native_dump(HANDLE process, const SharedReport& report, const std::filesystem::path& directory) {
    const auto native = find_native_dump(report.native_directory.data(), GetProcessId(process), process_creation_seconds(process));
    if (!native) return false;
    const auto temporary = directory / std::format(L"native-{}-{}-{}.tmp", native->process_id, native->process_created, native->thread_id);
    if (!CopyFileW(native->path.c_str(), temporary.c_str(), TRUE)) return false;
    return publish_dump(process, temporary, report, "game-native");
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
    // Uploading an old queue must never delay capturing a new crash. (One upload at a time: a
    // stuck load's report goes out while the game runs on, and the queue is the same.)
    std::mutex uploading;
    const auto upload = [&] { std::lock_guard lock(uploading); upload_pending(url, directory); };
    std::jthread uploader(upload);
    std::vector<std::jthread> stuck_uploads;
    SetEvent(handles[ready].value);
    const HANDLE wait_for[]{handles[requested].value, handles[stopping].value, handles[parent].value};
    auto reason = WaitForMultipleObjects(3, wait_for, FALSE, INFINITE);
    // A stuck load: the game goes on, so after its dump this goes back to waiting for the end.
    while (reason == WAIT_OBJECT_0 && shared->source == CaptureSource::stuck_load) {
        bool stuck_saved{};
        try { stuck_saved = save_dump(handles[parent].value, *shared, directory); } catch (...) {}
        // (Down before the answer: the game may ask again, or crash, as soon as it has it.)
        ResetEvent(handles[requested].value);
        SetEvent(handles[captured].value);
        if (stuck_saved) stuck_uploads.emplace_back(upload);
        reason = WaitForMultipleObjects(3, wait_for, FALSE, INFINITE);
    }
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
    if (saved) upload();
    return saved ? 0 : 4;
}
}
int dingosdk::crash_report::run_reporter(int argc, wchar_t** argv) noexcept {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try { return run(argc, argv); } catch (...) { return 1; }
}
