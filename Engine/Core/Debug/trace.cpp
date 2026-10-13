#include <Windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include "trace.h"
#include "protocol.h"
#include "crash_report_config.h"
#include "Engine/Core/Json/json.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace dingosdk::trace {
namespace {
constexpr std::size_t max_queued = 64;

struct Internet {
    HINTERNET value{};
    ~Internet() { if (value) WinHttpCloseHandle(value); }
};
struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::string> queue;
    bool worker{}, sending{};
    bool enabled{};
    std::wstring host, resource;
    INTERNET_PORT port{};
    std::string trace_id, process;
};
std::string random_hex(std::size_t bytes) {
    std::array<unsigned char, 16> buffer{};
    if (BCryptGenRandom(nullptr, buffer.data(), static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw std::runtime_error("No random source");
    std::string text;
    for (std::size_t i = 0; i < bytes; ++i) text += std::format("{:02x}", buffer[i]);
    return text;
}
std::string narrow(std::wstring_view value) {
    std::string result;
    for (const auto c : value) result += c < 0x80 ? static_cast<char>(c) : '?';
    return result;
}
std::string timestamp(Clock::time_point time) {
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(time));
}
void configure(State& s) {
    using crash_report::environment;
    if (environment(L"RESKATE_TRACING") != L"1" || environment(L"RESKATE_CRASH_REPORTING") == L"0") return;
    auto url = environment(L"RESKATE_CRASH_REPORT_URL");
    if (url.empty()) url = crash_report::configured_url;
    url = envelope_url(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (url.empty() || !WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &parts) ||
            parts.nScheme != INTERNET_SCHEME_HTTPS || !parts.dwHostNameLength) return;
    s.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    s.resource.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) s.resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    s.port = parts.nPort;
    // The game inherits the launcher's trace, so one launch reads as one timeline.
    s.trace_id = narrow(environment(L"RESKATE_TRACE_ID"));
    if (s.trace_id.size() != 32 || s.trace_id.find_first_not_of("0123456789abcdef") != std::string::npos) {
        s.trace_id = random_hex(16);
        SetEnvironmentVariableW(L"RESKATE_TRACE_ID", std::wstring(s.trace_id.begin(), s.trace_id.end()).c_str());
    }
    std::array<wchar_t, 32768> path{};
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length && length < path.size()) s.process = narrow(std::filesystem::path(path.data()).filename().wstring());
    s.enabled = true;
}
// Set by start() and never destroyed: the worker thread may outlive static
// destruction, and a DLL must not join a thread while the loader lock is held.
std::atomic<State*> active{};
constexpr std::uint64_t hitch_ns = 50ull * 1000 * 1000;
struct Pacing {
    std::atomic<std::uint64_t> last{}, frames{}, total{}, hitches{}, worst{};
} pacing;
void post(const State& s, const std::string& body) {
    Internet session{WinHttpOpen(L"ReSkate-Trace/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)};
    if (!session.value || !WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000)) return;
    Internet connection{WinHttpConnect(session.value, s.host.c_str(), s.port, 0)};
    if (!connection.value) return;
    Internet request{WinHttpOpenRequest(connection.value, L"POST", s.resource.c_str(), nullptr, nullptr,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.value) return;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    constexpr wchar_t headers[] = L"Content-Type: application/x-sentry-envelope\r\n";
    // A timing that cannot be delivered is dropped; nothing is retried or kept on disk.
    if (WinHttpSendRequest(request.value, headers, static_cast<DWORD>(-1), const_cast<char*>(body.data()),
            static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0))
        WinHttpReceiveResponse(request.value, nullptr);
}
void work(State& s) noexcept {
    std::unique_lock lock(s.mutex);
    for (;;) {
        s.changed.wait(lock, [&] { return !s.queue.empty(); });
        const auto body = std::move(s.queue.front());
        s.queue.pop_front();
        s.sending = true;
        lock.unlock();
        try { post(s, body); } catch (...) {}
        lock.lock();
        s.sending = false;
        s.changed.notify_all();
    }
}
}
std::wstring envelope_url(std::wstring_view minidump_url) {
    constexpr std::wstring_view minidump = L"/minidump/";
    const auto at = minidump_url.find(minidump);
    if (at == std::wstring_view::npos) return {};
    return std::wstring(minidump_url.substr(0, at)) + L"/envelope/" + std::wstring(minidump_url.substr(at + minidump.size()));
}
std::string envelope(const Transaction& transaction) {
    const auto event_id = random_hex(16);
    Json trace = Json::object();
    trace["trace_id"] = transaction.trace_id;
    trace["span_id"] = random_hex(8);
    trace["op"] = transaction.operation;
    trace["status"] = transaction.ok ? "ok" : "internal_error";
    Json contexts = Json::object();
    contexts["trace"] = trace;
    Json info = Json::object();
    info["source"] = "custom";
    Json tags = Json::object();
    tags["process"] = transaction.process;
    for (const auto& [key, value] : transaction.tags) tags[key] = value.substr(0, 200);
    Json measurements = Json::object();
    for (const auto& measurement : transaction.measurements) {
        Json entry = Json::object();
        entry["value"] = measurement.value;
        entry["unit"] = measurement.unit;
        measurements[measurement.name] = entry;
    }
    Json event = Json::object();
    event["type"] = "transaction";
    event["event_id"] = event_id;
    event["platform"] = "native";
    event["transaction"] = transaction.name;
    event["transaction_info"] = info;
    event["start_timestamp"] = timestamp(transaction.started);
    event["timestamp"] = timestamp(transaction.finished);
    event["release"] = transaction.release;
    event["contexts"] = contexts;
    event["tags"] = tags;
    if (!transaction.measurements.empty()) event["measurements"] = measurements;
    event["spans"] = Json::array();
    Json header = Json::object();
    header["event_id"] = event_id;
    header["sent_at"] = timestamp(Clock::now());
    const auto payload = event.dump();
    Json item = Json::object();
    item["type"] = "transaction";
    item["length"] = payload.size();
    return header.dump() + "\n" + item.dump() + "\n" + payload + "\n";
}
bool start() noexcept {
    try {
        if (active.load()) return true;
        auto s = std::make_unique<State>();
        configure(*s);
        if (!s->enabled) return false;
        State* expected{};
        if (active.compare_exchange_strong(expected, s.get())) (void)s.release();
        return true;
    } catch (...) { return false; }
}
void frame(std::uint64_t now_ns) noexcept {
    if (!active.load(std::memory_order_relaxed)) return;
    const auto previous = pacing.last.exchange(now_ns, std::memory_order_relaxed);
    if (!previous || now_ns <= previous) return;
    const auto took = now_ns - previous;
    pacing.frames.fetch_add(1, std::memory_order_relaxed);
    pacing.total.fetch_add(took, std::memory_order_relaxed);
    if (took >= hitch_ns) pacing.hitches.fetch_add(1, std::memory_order_relaxed);
    if (took > pacing.worst.load(std::memory_order_relaxed)) pacing.worst.store(took, std::memory_order_relaxed);
}
FramePacing take_frame_pacing() noexcept {
    pacing.last.store(0, std::memory_order_relaxed); // the next frame starts a fresh interval
    return {pacing.frames.exchange(0), pacing.total.exchange(0), pacing.hitches.exchange(0), pacing.worst.exchange(0)};
}
void record(std::string_view name, std::string_view operation, std::chrono::milliseconds duration,
        bool ok, Tags tags, Measurements measurements) noexcept {
    try {
        auto* const started = active.load();
        if (!started) return;
        auto& s = *started;
        Transaction transaction;
        transaction.name = name;
        transaction.operation = operation;
        transaction.trace_id = s.trace_id;
        transaction.release = std::string("reskate@") + crash_report::build_version;
        transaction.process = s.process;
        transaction.finished = Clock::now();
        transaction.started = transaction.finished - duration;
        transaction.ok = ok;
        transaction.tags = std::move(tags);
        transaction.measurements = std::move(measurements);
        auto body = envelope(transaction);
        std::lock_guard lock(s.mutex);
        if (s.queue.size() >= max_queued) return;
        s.queue.push_back(std::move(body));
        if (!s.worker) {
            std::thread([&s] { work(s); }).detach();
            s.worker = true;
        }
        s.changed.notify_all();
    } catch (...) {}
}
void flush(std::chrono::milliseconds timeout) noexcept {
    try {
        auto* const started = active.load();
        if (!started) return;
        auto& s = *started;
        std::unique_lock lock(s.mutex);
        s.changed.wait_for(lock, timeout, [&] { return s.queue.empty() && !s.sending; });
    } catch (...) {}
}
}
