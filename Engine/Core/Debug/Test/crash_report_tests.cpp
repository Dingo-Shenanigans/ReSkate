#include <Windows.h>
#include <DbgHelp.h>
#include "Engine/Core/Debug/crash_report.h"
#include "Engine/Core/Debug/native_dump.h"
#include "Engine/Core/Debug/upload.h"
#include "Engine/Core/Debug/report_envelope.h"
#include "Engine/Core/Debug/report_details.h"
#include "Engine/Core/Debug/protocol.h"
#include "Engine/Core/Debug/trace.h"
#include "crash_report_config.h"
#include "Engine/Core/Log/logging.h"
#include <array>
#include <fstream>
#include <format>
#include <iostream>
#include <thread>
#include <vector>

using namespace dingosdk;
using namespace dingosdk::crash_report;
namespace {
constexpr DWORD test_exception = 0xe0424242;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
LONG WINAPI finish_child(EXCEPTION_POINTERS*) { ExitProcess(42); }
std::filesystem::path native_fixture_directory;
LONG WINAPI native_child_handler(EXCEPTION_POINTERS* pointers) {
    const auto path = native_fixture_directory / L"game-crash.mdmp";
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), pointers, FALSE};
    if (file.value == INVALID_HANDLE_VALUE || !MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
            file.value, MiniDumpNormal, &exception, nullptr, nullptr)) ExitProcess(43);
    CloseHandle(file.release());
    CopyFileW(path.c_str(), (native_fixture_directory / L"duplicate.dmp").c_str(), TRUE);
    ExitProcess(42);
}
// A heap block only the faulting stack points at: present in a dump only with the memory the
// stacks reference. Made at run time, so its bytes are nowhere else in the process.
// (Filled in place: a copy left on a stack would be in every dump.)
__declspec(noinline) void fill_marker(std::array<char, 64>& bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>((i * 37 + 11) ^ 0xa5);
}
// The game's own fatal handler: ReSkate's import hook asks for its dump first, then the game
// writes its own (game_fatal_dump.cpp).
LONG WINAPI fatal_child_handler(EXCEPTION_POINTERS* pointers) {
    capture_game_fatal(GetCurrentThreadId(), pointers);
    return native_child_handler(pointers);
}
enum class Mode { direct, native, fatal };
std::string read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void endpoint_checks() {
    Endpoint endpoint;
    check(parse_endpoint(L"https://sentry.example.com/api/2/minidump/?sentry_key=0123abcd", endpoint), "Hosted endpoint rejected");
    check(endpoint.host == L"sentry.example.com" && endpoint.port == 443 && endpoint.resource == L"/api/2/minidump/?sentry_key=0123abcd", "Hosted URL components incorrect");
    check(parse_endpoint(L"https://sentry.example.com:9000/api/2/minidump/?sentry_key=0123abcd", endpoint), "Endpoint with a port rejected");
    check(endpoint.port == 9000 && endpoint.resource == L"/api/2/minidump/?sentry_key=0123abcd", "Endpoint port or key lost");
    for (const auto* value : {L"", L"http://example.com/post", L"https://user:password@example.com/post",
            L"https://example.com/post#fragment", L"https://example.com/post\r\nheader", L"https:///post", L"https://example.com"})
        check(!parse_endpoint(value, endpoint), "Unsafe endpoint accepted");
    std::string id;
    const std::string receipt = "c350c4ee-5e2b-4b77-9b2e-5e4a2b0e1b9d";
    check(accepted_response(200, receipt + "\n", id) && id == receipt, "Valid receipt rejected");
    check(accepted_response(200, "c350c4ee5e2b4b779b2e5e4a2b0e1b9d", id), "Unhyphenated receipt rejected");
    for (const auto* response : {"", "\n", "<html>OK</html>", R"({"detail":"ok"})", "c350c4ee-5e2b-4b77-9b2e", "c350c4ee-5e2b-4b77-9b2e-5e4a2b0e1b9d-00"})
        check(!accepted_response(200, response, id), "Unconfirmed submission accepted");
    check(accepted_response(200, R"({"id":"c350c4ee5e2b4b779b2e5e4a2b0e1b9d"})", id) && id == "c350c4ee5e2b4b779b2e5e4a2b0e1b9d",
        "Envelope receipt rejected");
    check(!accepted_response(200, R"({"id":"short"})", id) && !accepted_response(200, R"({"id":)", id), "Broken envelope receipt accepted");
    check(envelope_endpoint(L"https://sentry.example.com/api/2/minidump/?sentry_key=0123abcd") ==
        L"https://sentry.example.com/api/2/envelope/?sentry_key=0123abcd" && envelope_endpoint(L"https://example.com/post").empty(),
        "Crash envelope endpoint not derived from the minidump endpoint");
    check(!accepted_response(429, receipt, id), "Rate limited submission accepted");
    check(!accepted_response(500, receipt, id), "Failed submission accepted");
}
void trace_checks() {
    check(trace::envelope_url(L"https://sentry.example.com/api/2/minidump/?sentry_key=0123abcd") ==
        L"https://sentry.example.com/api/2/envelope/?sentry_key=0123abcd", "Envelope endpoint not derived from the minidump endpoint");
    check(trace::envelope_url(L"https://example.com/post").empty(), "Timings would be sent to an endpoint that is not Sentry's");
    trace::Transaction transaction;
    transaction.name = "game.level_load"; transaction.operation = "app.load";
    transaction.trace_id = "0123456789abcdef0123456789abcdef"; transaction.release = "reskate@fixture-build";
    transaction.process = "Skate.exe"; transaction.ok = false;
    transaction.finished = trace::Clock::now(); transaction.started = transaction.finished - std::chrono::milliseconds(1500);
    transaction.tags = {{"destination", std::string(300, 'x')}};
    transaction.measurements = {{"fps_avg", 59.5}, {"worst_frame", 120.0, "millisecond"}};
    const auto wire = trace::envelope(transaction);
    const auto first = wire.find('\n'), second = wire.find('\n', first + 1);
    check(first != std::string::npos && second != std::string::npos && wire.back() == '\n', "Envelope is not three lines");
    const auto header = Json::parse(wire.substr(0, first)), item = Json::parse(wire.substr(first + 1, second - first - 1));
    const auto payload = wire.substr(second + 1, wire.size() - second - 2);
    const auto event = Json::parse(payload);
    check(item.at("type").string() == "transaction" && item.at("length").get<std::size_t>() == payload.size(), "Envelope item header wrong");
    check(header.at("event_id").string() == event.at("event_id").string() && event.at("event_id").string().size() == 32, "Event ID missing");
    const auto& context = event.at("contexts").at("trace");
    check(context.at("trace_id").string() == transaction.trace_id && context.at("span_id").string().size() == 16 &&
        context.at("op").string() == "app.load" && context.at("status").string() == "internal_error", "Trace context wrong");
    check(event.at("transaction").string() == "game.level_load" && event.at("release").string() == "reskate@fixture-build" &&
        event.at("tags").at("process").string() == "Skate.exe" && event.at("tags").at("destination").string().size() == 200, "Transaction fields wrong");
    check(event.at("measurements").at("fps_avg").at("value").get<double>() == 59.5 &&
        event.at("measurements").at("worst_frame").at("unit").string() == "millisecond", "Measurements wrong");
    check(event.at("start_timestamp").string() < event.at("timestamp").string() && event.at("timestamp").string().back() == 'Z', "Timestamps wrong");
    SetEnvironmentVariableW(L"RESKATE_CRASH_REPORTING", L"0");
    check(!trace::start(), "Timings sent with crash reporting off");
    trace::frame(1000000000);
    check(trace::take_frame_pacing().frames == 0, "Frame pacing counted while timings are off");
}
void attachment_checks(const std::filesystem::path& root) {
    const auto directory = root / L"attachments";
    std::filesystem::create_directories(directory);
    const auto log = directory / L"ReSkate.log", dump = directory / L"fixture.dmp";
    const std::string dump_bytes("MDMP\0binary\xff", 12);
    const std::string original_log = "session start\r\nlast crash message\r\n";
    std::ofstream(dump, std::ios::binary).write(dump_bytes.data(), dump_bytes.size());
    std::ofstream(log, std::ios::binary) << original_log;
    check(snapshot_log(log, dump) == LogSnapshot::complete, "Log snapshot failed");
    std::ofstream(log) << "a later game session";
    check(read(log_attachment_path(dump)) == original_log, "Queued attachment changed with live log");
    auto attributes = Json::object(); attributes["version"] = "fixture-build"; attributes["attachment.log"] = "complete";
    std::ofstream(event_details_path(dump), std::ios::binary)
        << R"({"breadcrumbs":{"values":[{"message":"last crash message"}]},"user":{"id":"0123456789abcdef0123456789abcdef"},)"
           R"("contexts":{"reskate":{"mods":["A 1.0"]}},"tags":{"version":"replaced"},"release":"replaced"})";
    const std::string event_id = "0123456789abcdef0123456789abcdef";
    {
        ReportEnvelope body(dump, attributes, event_id);
        check(body.error() == 0, "Cannot prepare attached report");
        std::string wire;
        check(body.write([&](const void* data, DWORD bytes) { wire.append(static_cast<const char*>(data), bytes); return true; }), "Cannot stream attached report");
        check(wire.size() == body.size(), "Envelope Content-Length does not match transmitted bytes");
        // Read the envelope back independently: a header line, then each item's header line and
        // exactly `length` bytes of payload, each followed by a newline.
        std::size_t cursor = wire.find('\n');
        check(cursor != std::string::npos, "Envelope has no header line");
        const auto header = Json::parse(wire.substr(0, cursor));
        check(header.at("event_id").string() == event_id, "Envelope header lacks the event ID");
        ++cursor;
        std::vector<std::pair<Json, std::string>> items;
        while (cursor < wire.size()) {
            const auto line_end = wire.find('\n', cursor);
            check(line_end != std::string::npos, "Unterminated item header");
            auto item = Json::parse(wire.substr(cursor, line_end - cursor));
            const auto length = item.at("length").get<std::size_t>();
            check(line_end + 1 + length <= wire.size(), "Item longer than the envelope");
            items.emplace_back(std::move(item), wire.substr(line_end + 1, length));
            cursor = line_end + 1 + length;
            check(cursor == wire.size() || wire[cursor] == '\n', "Item payload not followed by a newline");
            if (cursor < wire.size()) ++cursor;
        }
        check(items.size() == 4, "Envelope does not hold the event, dump, report and log");
        const auto& [event_item, event_payload] = items[0];
        const auto event = Json::parse(event_payload);
        check(event_item.at("type").string() == "event" && event.at("event_id").string() == event_id &&
            event.at("platform").string() == "native" && event.at("level").string() == "fatal" &&
            event.at("timestamp").get<double>() > 1.7e9, "Event item wrong");
        check(event.at("tags").at("version").string() == "fixture-build" &&
            event.at("release").string() == "reskate@fixture-build", "Attributes lost from the event");
        check(event.at("breadcrumbs").at("values")[0].at("message").string() == "last crash message" &&
            event.at("user").at("id").string() == "0123456789abcdef0123456789abcdef" &&
            event.at("contexts").at("reskate").at("mods")[0].string() == "A 1.0", "Event details did not reach the event");
        check(items[1].first.at("type").string() == "attachment" && items[1].first.at("attachment_type").string() == "event.minidump" &&
            items[1].second == dump_bytes, "Dump not the event's minidump, or its bytes corrupted");
        check(items[2].first.at("filename").string() == "report.json" &&
            Json::parse(items[2].second).at("version").string() == "fixture-build", "Report details attachment missing");
        check(items[3].first.at("filename").string() == "ReSkate.log" && items[3].second == original_log, "Log is not a named file attachment");
    }
    check(ReportEnvelope(dump, attributes, "not-an-id").error() != 0, "An envelope without a proper event ID");
    {
        ReportEnvelope body(dump, attributes, event_id);
        check(!body.write([](const void*, DWORD) { return false; }), "Stream failure ignored");
    }
    std::filesystem::remove(log_attachment_path(dump));
    check(ReportEnvelope(dump, attributes, event_id).error() != 0, "Missing queued log silently dropped");
    attributes.erase("attachment.log");
    check(ReportEnvelope(dump, attributes, event_id).error() == 0, "Legacy report without logs blocked");
    const std::string tail(max_log_bytes, 'x');
    std::ofstream(log, std::ios::binary) << "discard this old prefix" << tail;
    check(snapshot_log(log, dump) == LogSnapshot::tail && read(log_attachment_path(dump)) == tail, "Large log did not retain bounded tail");
    check(std::filesystem::file_size(log) > max_log_bytes, "Original log was truncated");
    check(snapshot_log(directory / L"missing.log", directory / L"missing.dmp") == LogSnapshot::unavailable, "Missing source log reported as attached");
}
void details_checks(const std::filesystem::path& root) {
    const auto directory = root / L"details";
    std::filesystem::create_directories(directory);
    const auto log = directory / L"ReSkate.log";
    std::ofstream(log, std::ios::binary)
        << "[2026-10-16 16:56:01.000] [1:2] SDK: [Runtime] [DEBUG] old debug line\r\n"
        << "[2026-10-16 16:56:02.000] [1:2] Native(C): [Level] [INFO] Level active [13]\r\n"
        << "[2026-10-16 16:56:03.000] [1:2] Native(M): [Graphics] [WARNING] Descriptor heap nearly full\r\n"
        << "[2026-10-16 16:56:04.000] [1:2] SDK: [Runtime] [DEBUG] recent debug line\r\n"
        << "a continuation without a time \xff\r\n";
    const auto crumbs = log_breadcrumbs(log, 10, 2);
    const auto& values = crumbs.at("values");
    check(values.size() == 4, "Breadcrumbs kept a debug line from before the last few, or lost one");
    check(values[0].at("message").string() == "Level active [13]" && values[0].at("level").string() == "info" &&
        values[0].at("category").string() == "Native(C)/Level", "Breadcrumb text, level or category wrong");
    check(values[1].at("level").string() == "warning" && values[2].at("level").string() == "debug", "Breadcrumb levels wrong");
    check(values[3].at("message").string() == "a continuation without a time ?" &&
        values[3].at("timestamp").get<double>() == values[2].at("timestamp").get<double>(), "A line without a time not kept with the one before");
    check(values[1].at("timestamp").get<double>() - values[0].at("timestamp").get<double>() == 1.0 &&
        values[0].at("timestamp").get<double>() > 1.7e9, "Breadcrumb times wrong");
    check(log_breadcrumbs(directory / L"missing.log").is_null(), "Breadcrumbs from a missing log");
    const auto id = install_id(directory);
    check(id.size() == 32 && install_id(directory) == id, "Install ID not made once and kept");
    check(crash_site(log).empty(), "A crash site read from something that is not a dump");
}
void queue_checks(const std::filesystem::path& root) {
    const auto queue = root / L"queue";
    std::filesystem::create_directories(queue);
    const auto dump = queue / L"fixture.dmp";
    const auto metadata = dump.wstring() + L".json";
    std::ofstream(dump) << "synthetic test data";
    std::ofstream(metadata) << R"({"version":"original-build","attachment.log":"complete"})";
    std::ofstream(log_attachment_path(dump)) << "original-session-log";
    const Sender failure = [](const auto&, const auto&, const auto&) { return UploadResult{false, 503, 0, {}}; };
    check(upload_pending(L"test", queue, failure) == 0 && std::filesystem::exists(dump), "Failed upload did not retain dump");
    check(read(log_attachment_path(dump)) == "original-session-log", "Failed upload lost log attachment");
    const Sender success = [](const auto&, const auto& queued_dump, const Json& attributes) {
        check(attributes.at("version").string() == "original-build", "Queued report lost original build attributes");
        check(read(log_attachment_path(queued_dump)) == "original-session-log", "Retry lost original session log");
        return UploadResult{true, 200, 0, "c350c4ee5e2b4b779b2e5e4a2b0e1b9d"};
    };
    Handle lock(CreateFileW((queue / L"queue.lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    check(upload_pending(L"test", queue, success) == 0 && std::filesystem::exists(dump), "Queue lock failed");
    CloseHandle(lock.release());
    check(upload_pending(L"test", queue, success) == 1, "Confirmed upload not counted");
    check(!std::filesystem::exists(dump) && !std::filesystem::exists(metadata) && !std::filesystem::exists(log_attachment_path(dump)), "Confirmed report or attachment not removed");
    const auto sent = queue / L"sent" / L"fixture.dmp";
    check(std::filesystem::exists(sent) && read(log_attachment_path(sent)) == "original-session-log" &&
        std::filesystem::exists(sent.wstring() + L".json"), "Confirmed report not kept in sent");
    for (std::size_t i = 0; i < kept_sent_reports + 3; ++i) {
        const auto path = queue / std::format(L"sent-{}.dmp", i);
        std::ofstream(path) << "synthetic test data";
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(i + 1));
        keep_sent_report(path);
    }
    std::size_t kept{};
    for (const auto& entry : std::filesystem::directory_iterator(queue / L"sent")) kept += entry.path().extension() == L".dmp";
    check(kept == kept_sent_reports && !std::filesystem::exists(sent) && std::filesystem::exists(queue / L"sent" / L"sent-12.dmp"),
        "Sent reports not pruned to the newest");
    check(queue_directory(root, L"https://a/token/minidump") != queue_directory(root, L"https://b/token/minidump"), "Different destinations share queue");
    for (std::size_t i = 0; i < max_pending_reports + 2; ++i) {
        const auto path = queue / std::format(L"{}.dmp", i);
        std::ofstream(path) << "synthetic test data";
        std::ofstream(path.wstring() + L".json") << R"({"version":"original-build"})";
        std::ofstream(log_attachment_path(path)) << "retention-fixture";
    }
    upload_pending(L"test", queue, failure);
    std::size_t count{};
    for (const auto& entry : std::filesystem::directory_iterator(queue)) {
        if (entry.path().extension() == L".dmp") ++count;
        if (entry.path().extension() == L".log") {
            auto owner = entry.path(); owner.replace_extension();
            check(std::filesystem::exists(owner), "Retention orphaned a log attachment");
        }
    }
    check(count == max_pending_reports, "Queue retention limit not enforced");
    const auto expired = queue / L"expired.dmp";
    std::ofstream(expired) << "expired";
    std::ofstream(expired.wstring() + L".json") << "{}";
    std::ofstream(log_attachment_path(expired)) << "expired-session-log";
    std::filesystem::last_write_time(expired, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 15));
    upload_pending(L"test", queue, failure);
    check(!std::filesystem::exists(expired) && !std::filesystem::exists(log_attachment_path(expired)), "Expired report or attachment retained");
}
void crash_check(const std::filesystem::path& root, Mode mode) {
    const bool native = mode != Mode::direct;
    std::filesystem::create_directories(root);
    std::array<wchar_t, 32768> executable{};
    GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    std::filesystem::path child_path(executable.data());
    if (native) {
        child_path = root / L"Skate.exe";
        std::filesystem::copy_file(executable.data(), child_path);
        std::filesystem::copy_file(std::filesystem::path(executable.data()).parent_path() / L"ReSkateLauncher.exe", root / L"ReSkateLauncher.exe");
    }
    auto command = std::format(L"\"{}\" {} \"{}\"", child_path.wstring(),
        mode == Mode::fatal ? L"--fatal-crash" : native ? L"--native-crash" : L"--crash", root.wstring());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    check(CreateProcessW(child_path.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "Cannot start crash fixture");
    Handle child(process.hProcess), thread(process.hThread);
    check(WaitForSingleObject(child.value, 30000) == WAIT_OBJECT_0, "Crash fixture timed out");
    DWORD code{}; GetExitCodeProcess(child.value, &code);
    check(code == 42, "Previous exception handler was not preserved");
    const auto log = read(root / L"ReSkate.log");
    check(log.find("Crash reporting enabled") != std::string::npos, "Helper did not start");
    check((log.find("ReSkate unhandled exception") != std::string::npos) != native, "Fixture did not exercise the expected exception handler");
    const auto queue = queue_directory(root / L"crashes", L"https://127.0.0.1:1/test/minidump");
    std::filesystem::path dump;
    for (int attempt = 0; attempt < 100 && dump.empty(); ++attempt) {
        for (const auto& entry : std::filesystem::directory_iterator(queue)) if (entry.path().extension() == L".dmp") dump = entry.path();
        if (dump.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(!dump.empty(), "No out-of-process minidump captured");
    auto bytes = read(dump);
    MINIDUMP_DIRECTORY* stream{}; void* data{}; ULONG size{};
    check(MiniDumpReadDumpStream(bytes.data(), ExceptionStream, &stream, &data, &size) != FALSE, "Minidump exception stream missing");
    const auto* exception = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(data);
    check(size >= sizeof(*exception) && exception->ExceptionRecord.ExceptionCode == test_exception, "Wrong exception captured");
    check(MiniDumpReadDumpStream(bytes.data(), ModuleListStream, &stream, &data, &size) != FALSE, "Minidump module list missing");
    check(Json::parse(read(dump.wstring() + L".json")).at("application").string() == "ReSkate", "Crash attributes missing");
    check(Json::parse(read(dump.wstring() + L".json")).at("capture.source").string() ==
        (mode == Mode::fatal ? "game-fatal-handler" : native ? "game-native" : "unhandled-exception"), "Wrong capture source");
    if (mode != Mode::native) {
        std::array<char, 64> marker{};
        fill_marker(marker);
        check(bytes.find(std::string_view(marker.data(), marker.size())) != std::string::npos,
            "Dump lacks the heap memory the faulting stack points at");
    }
    check(Json::parse(read(dump.wstring() + L".json")).at("attachment.log").string() == "complete" &&
        read(log_attachment_path(dump)) == log, "Crash did not snapshot its session log");
    const auto tags = Json::parse(read(dump.wstring() + L".json"));
    const auto site = tags.at("crash.site").string();
    // (The fixture raises its exception, so it happens in RaiseException.)
    check(site.starts_with("KERNELBASE.dll+0x") && site.size() > 17, "Crash site not tagged");
    const auto details = Json::parse(read(event_details_path(dump)));
    check(details.at("user").at("id").string().size() == 32 && details.at("breadcrumbs").at("values").size() > 0 &&
        details.at("contexts").at("reskate").at("mods").is_array() && tags.at("mods.count").string() == "0",
        "Crash did not carry its event details");
    check(tags.at("game.map").string() == "dingolevel_reskate_lsc_b2" &&
        tags.at("game.destination").string() == "Levels/Game/DingoLevel_Root/DingoLevel_Root + levels/game/dingolevel_reskate_lsc_b2/dingolevel_reskate_lsc_b2 @ DefaultStartPoint" &&
        tags.at("gpu.name").string() == "Fixture GPU" && tags.at("gpu.driver").string() == "32.0.21025.1",
        "Crash was not tagged with its level and graphics card");
    if (mode == Mode::native) {
        const auto source = root / L"local" / L"ReSkate" / L"Game" / L"Skate" / L"CrashDumps";
        const auto pid = GetProcessId(child.value), created = process_creation_seconds(child.value);
        const auto selected = find_native_dump(source, pid, created);
        check(selected && selected->path.extension() == L".mdmp", "Native dump selection failed");
        check(!find_native_dump(source, pid + 1, created), "Imported a different process's dump");
        check(!find_native_dump(source, pid, created - 1), "Imported a different lifetime of the same PID");
        check(std::filesystem::exists(source / L"game-crash.mdmp"), "Original game dump was removed");
        std::filesystem::create_directories(root / L"invalid");
        std::ofstream(root / L"invalid" / L"bad.mdmp") << "MDMP";
        check(!find_native_dump(root / L"invalid", pid, created), "Truncated native dump accepted");
    }
    // Allow the helper's deliberately refused loopback upload to finish before fixture cleanup.
    for (int i = 0; i < 100 && !std::filesystem::exists(queue / L"status.txt"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(std::filesystem::exists(dump), "Offline dump was lost");
}
// A stuck load: a dump of the running fixture, after which it goes on and crashes for real. Both
// are captured, the first as a stuck-load report.
void stuck_check(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    std::array<wchar_t, 32768> executable{};
    GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    auto command = std::format(L"\"{}\" --stuck \"{}\"", executable.data(), root.wstring());
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    check(CreateProcessW(executable.data(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "Cannot start stuck fixture");
    Handle child(process.hProcess), thread(process.hThread);
    check(WaitForSingleObject(child.value, 60000) == WAIT_OBJECT_0, "Stuck fixture timed out");
    DWORD code{}; GetExitCodeProcess(child.value, &code);
    check(code != 44, "The stuck-load capture was not answered");
    check(code == 42, "The fixture did not go on to its crash after the stuck-load capture");
    const auto queue = queue_directory(root / L"crashes", L"https://127.0.0.1:1/test/minidump");
    std::filesystem::path stuck, crash;
    for (int attempt = 0; attempt < 100 && (stuck.empty() || crash.empty()); ++attempt) {
        for (const auto& entry : std::filesystem::directory_iterator(queue)) {
            if (entry.path().extension() != L".dmp") continue;
            const auto source = Json::parse(read(entry.path().wstring() + L".json")).at("capture.source").string();
            (source == "stuck-load" ? stuck : crash) = entry.path();
        }
        if (stuck.empty() || crash.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    check(!stuck.empty(), "No stuck-load dump captured");
    check(!crash.empty(), "The crash after a stuck-load capture was not captured");
    check(Json::parse(read(crash.wstring() + L".json")).at("capture.source").string() == "unhandled-exception", "Wrong capture source after a stuck load");
    auto bytes = read(stuck);
    MINIDUMP_DIRECTORY* stream{}; void* data{}; ULONG size{};
    check(MiniDumpReadDumpStream(bytes.data(), ExceptionStream, &stream, &data, &size) != FALSE &&
        static_cast<const MINIDUMP_EXCEPTION_STREAM*>(data)->ExceptionRecord.ExceptionCode == stuck_load_exception,
        "Stuck-load dump lacks its exception");
    check(MiniDumpReadDumpStream(bytes.data(), ThreadListStream, &stream, &data, &size) != FALSE &&
        static_cast<const MINIDUMP_THREAD_LIST*>(data)->NumberOfThreads >= 2, "Stuck-load dump lacks the other threads");
    const auto tags = Json::parse(read(stuck.wstring() + L".json"));
    check(tags.at("error.type").string() == "stall" && tags.at("load.stage").string() == "Starting level load" &&
        tags.at("load.seconds").string() == "130" && !tags.contains("crash.site") &&
        tags.at("game.map").string() == "dingolevel_reskate_lsc_b2", "Stuck-load report not tagged as one");
    const auto details = Json::parse(read(event_details_path(stuck)));
    check(details.at("level").string() == "warning" && details.at("fingerprint").size() == 3 &&
        details.at("fingerprint")[0].string() == "stuck-load" && details.at("fingerprint")[2].string() == "dingolevel_reskate_lsc_b2",
        "Stuck-load report not grouped by its stage and map");
    ReportEnvelope body(stuck, tags, "0123456789abcdef0123456789abcdef");
    check(body.error() == 0, "Cannot prepare a stuck-load report");
    std::string wire;
    check(body.write([&](const void* chunk, DWORD count) { wire.append(static_cast<const char*>(chunk), count); return true; }), "Cannot stream a stuck-load report");
    check(wire.find("\"level\":\"warning\"") != std::string::npos && wire.find("\"fingerprint\":[\"stuck-load\"") != std::string::npos,
        "A stuck-load report would be sent as a fatal crash");
    for (int i = 0; i < 100 && !std::filesystem::exists(queue / L"status.txt"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
}
int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try {
        if (argc == 5 && std::wstring_view(argv[1]) == L"--inspect-native") {
            const auto dump = find_native_dump(argv[2], static_cast<DWORD>(std::stoul(argv[3])), static_cast<DWORD>(std::stoul(argv[4])));
            check(dump.has_value(), "No native dump matched this process lifetime");
            std::cout << "Matched native crash: process " << dump->process_id << "; exception 0x" << std::hex
                << dump->exception_code << "; address 0x" << dump->exception_address << '\n';
            return 0;
        }
        if (argc == 2 && std::wstring_view(argv[1]) == L"--submit-trace") {
            SetEnvironmentVariableW(L"RESKATE_TRACING", L"1");
            check(trace::start(), "Timings are not configured for this build");
            trace::record("integration.test", "test", std::chrono::milliseconds(1234), true, {{"integration.test", "true"}},
                {{"fps_avg", 60.0}});
            trace::flush(std::chrono::seconds(15));
            std::cout << "Sent one synthetic transaction named integration.test; look for it in Sentry.\n";
            return 0;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--submit-fixture") {
            const std::filesystem::path dump(argv[2]);
            auto bytes = read(dump);
            check(bytes.size() >= sizeof(MINIDUMP_HEADER), "Not a minidump fixture");
            MINIDUMP_DIRECTORY* stream{}; void* data{}; ULONG size{};
            check(MiniDumpReadDumpStream(bytes.data(), ExceptionStream, &stream, &data, &size) != FALSE &&
                size >= sizeof(MINIDUMP_EXCEPTION_STREAM) &&
                static_cast<const MINIDUMP_EXCEPTION_STREAM*>(data)->ExceptionRecord.ExceptionCode == test_exception,
                "Only synthetic regression-test crashes may be submitted by this command");
            auto attributes = Json::parse(read(dump.wstring() + L".json"));
            attributes["integration.test"] = "true";
            attributes["error.message"] = "Synthetic ReSkate crash report integration test; no game crash";
            auto url = environment(L"RESKATE_CRASH_REPORT_URL");
            if (url.empty()) url = configured_url;
            const auto result = upload_report(url, dump, attributes);
            std::cout << "HTTP " << result.http_status << "; Windows error " << result.error << "; report " << result.report_id << '\n';
            check(result.accepted, "Sentry did not confirm test report");
            return 0;
        }
        if (argc == 3 && (std::wstring_view(argv[1]) == L"--crash" || std::wstring_view(argv[1]) == L"--native-crash" ||
                std::wstring_view(argv[1]) == L"--fatal-crash")) {
            const bool fatal = std::wstring_view(argv[1]) == L"--fatal-crash";
            const bool native = fatal || std::wstring_view(argv[1]) == L"--native-crash";
            if (native) {
                const auto local = std::filesystem::path(argv[2]) / L"local";
                SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
                native_fixture_directory = local / L"ReSkate" / L"Game" / L"Skate" / L"CrashDumps";
                std::filesystem::create_directories(native_fixture_directory);
            }
            SetEnvironmentVariableW(L"RESKATE_CRASH_REPORTING", L"1");
            SetEnvironmentVariableW(L"RESKATE_CRASH_REPORT_URL", L"https://127.0.0.1:1/test/minidump");
            SetUnhandledExceptionFilter(finish_child);
            logging::Options options; options.directory = argv[2]; options.truncate_file = true;
            check(logging::initialize(options), "Fixture logging failed");
            if (native) SetUnhandledExceptionFilter(fatal ? fatal_child_handler : native_child_handler); // Reproduce the game's later handler registration.
            set_destination(L"Levels/Game/DingoLevel_Root/DingoLevel_Root + levels/game/dingolevel_reskate_lsc_b2/dingolevel_reskate_lsc_b2 @ DefaultStartPoint");
            set_gpu(L"Fixture GPU", L"32.0.21025.1");
            auto* const volatile marker = new std::array<char, 64>;
            fill_marker(*marker);
            const ULONG_PTR arguments[]{reinterpret_cast<ULONG_PTR>(marker)};
            RaiseException(test_exception, EXCEPTION_NONCONTINUABLE, 1, arguments);
            return 3;
        }
        if (argc == 3 && std::wstring_view(argv[1]) == L"--stuck") {
            SetEnvironmentVariableW(L"RESKATE_CRASH_REPORTING", L"1");
            SetEnvironmentVariableW(L"RESKATE_CRASH_REPORT_URL", L"https://127.0.0.1:1/test/minidump");
            SetUnhandledExceptionFilter(finish_child);
            logging::Options options; options.directory = argv[2]; options.truncate_file = true;
            check(logging::initialize(options), "Fixture logging failed");
            set_destination(L"Levels/Game/DingoLevel_Root/DingoLevel_Root + levels/game/dingolevel_reskate_lsc_b2/dingolevel_reskate_lsc_b2 @ DefaultStartPoint");
            // (Another thread, as a load's job workers would be: the dump holds it too.)
            std::jthread worker([](std::stop_token stop) { while (!stop.stop_requested()) Sleep(10); });
            if (!capture_stuck_load(L"Starting level load", 130)) ExitProcess(44);
            // The game goes on: a second capture is refused only while one is in hand.
            worker.request_stop();
            worker.join();
            RaiseException(test_exception, EXCEPTION_NONCONTINUABLE, 0, nullptr);
            return 3;
        }
        check(argc == 2, "Pass a fixture root");
        trace_checks();
        const auto root = std::filesystem::path(argv[1]) / std::format(L"run-{}-{}", GetCurrentProcessId(), GetTickCount64());
        std::filesystem::create_directories(root);
        SetEnvironmentVariableW(L"RESKATE_CRASH_REPORTING", L"0");
        check(start(root) == StartResult::disabled, "Explicit opt-out ignored");
        SetEnvironmentVariableW(L"RESKATE_CRASH_REPORTING", L"1");
        SetEnvironmentVariableW(L"RESKATE_CRASH_REPORT_URL", L"http://invalid/post");
        check(start(root) == StartResult::failed, "Invalid endpoint enabled reporter");
        endpoint_checks();
        attachment_checks(root);
        details_checks(root);
        queue_checks(root);
        crash_check(root / L"direct", Mode::direct);
        crash_check(root / L"native", Mode::native);
        crash_check(root / L"fatal", Mode::fatal);
        stuck_check(root / L"stuck");
        std::cout << "Timing envelopes, endpoint, receipts, multipart attachments, log snapshots, retries, retention, opt-out, exception chaining, direct capture, native-handler fallback, game fatal-handler and stuck-load capture checks passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
