#pragma once
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dingosdk::trace {
using Tags = std::vector<std::pair<std::string, std::string>>;
using Clock = std::chrono::system_clock;
// A number carried by a transaction, such as an average frame rate.
struct Measurement { std::string name; double value{}; std::string unit{"none"}; };
using Measurements = std::vector<Measurement>;
// Called once logging is up: true when this launch sends timings. Until then,
// and in programs that never call it, record() does nothing. Timings are off
// unless RESKATE_TRACING=1 (the launcher's "Send performance timings" setting),
// and off whenever crash reporting is (RESKATE_CRASH_REPORTING=0).
bool start() noexcept;
// Whether this launch sends timings (start() succeeded).
bool active() noexcept;
// Queues one finished operation as a Sentry transaction. Never waits on the
// network and never throws. A launch's launcher and game share one trace.
void record(std::string_view name, std::string_view operation, std::chrono::milliseconds duration,
    bool ok = true, Tags tags = {}, Measurements measurements = {}) noexcept;
// Frame pacing between two takes: the game's frame hook feeds it, and the end
// of a level session takes it. A hitch is a frame of 50 ms or longer.
struct FramePacing { std::uint64_t frames{}, total_ns{}, hitches{}, worst_ns{}; };
void frame(std::uint64_t now_ns) noexcept;
FramePacing take_frame_pacing() noexcept;
// Gives queued operations a moment to reach the server before the process exits.
void flush(std::chrono::milliseconds timeout) noexcept;

// Times its own lifetime. An exception passing through marks the operation failed.
class Scope {
public:
    Scope(std::string name, std::string operation)
        : name_(std::move(name)), operation_(std::move(operation)) {}
    ~Scope() {
        record(name_, operation_, std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_), ok_ && std::uncaught_exceptions() == exceptions_, std::move(tags_));
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void tag(std::string key, std::string value) { tags_.emplace_back(std::move(key), std::move(value)); }
    void fail() noexcept { ok_ = false; }
private:
    std::string name_, operation_;
    Tags tags_;
    std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
    int exceptions_{std::uncaught_exceptions()};
    bool ok_{true};
};

struct Transaction {
    std::string name, operation, trace_id, release, process;
    Clock::time_point started, finished;
    bool ok{true};
    Tags tags;
    Measurements measurements;
};
// The Sentry envelope for one transaction: envelope header, item header, payload.
std::string envelope(const Transaction& transaction);
// The envelope endpoint beside a minidump endpoint, or empty when the URL is not one.
std::wstring envelope_url(std::wstring_view minidump_url);
}
