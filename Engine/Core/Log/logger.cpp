#include "logging_internal.h"
#include "Engine/Core/Debug/crash_report.h"
#include "Engine/Core/Debug/trace.h"
#include "Engine/Core/Console/console_core.h"
#include <spdlog/logger.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>

namespace dingosdk::logging {
namespace {
spdlog::level::level_enum native_level(Level level) noexcept {
    switch (level) {
    case Level::trace: return spdlog::level::trace;
    case Level::debug: return spdlog::level::debug;
    case Level::warning: return spdlog::level::warn;
    case Level::error: return spdlog::level::err;
    case Level::critical: return spdlog::level::critical;
    case Level::off: return spdlog::level::off;
    default: return spdlog::level::info;
    }
}
// FILE_APPEND_DATA chooses EOF for each WriteFile, including writes from the
// launcher while the runtime is initializing. No independent buffered offsets.
class SharedFile final : public spdlog::sinks::base_sink<std::mutex> {
public:
    SharedFile(const std::filesystem::path& path, bool truncate) {
        constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        if (truncate) {
            const auto reset = CreateFileW(path.c_str(), GENERIC_WRITE, sharing, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (reset == INVALID_HANDLE_VALUE) spdlog::throw_spdlog_ex("Cannot reset ReSkate.log", GetLastError());
            CloseHandle(reset);
        }
        file_ = CreateFileW(path.c_str(), FILE_APPEND_DATA, sharing, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) spdlog::throw_spdlog_ex("Cannot open ReSkate.log", GetLastError());
    }
    ~SharedFile() override { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(message, formatted);
        DWORD written{};
        if (!WriteFile(file_, formatted.data(), static_cast<DWORD>(formatted.size()), &written, nullptr))
            spdlog::throw_spdlog_ex("Cannot write ReSkate.log", GetLastError());
        if (written != formatted.size()) spdlog::throw_spdlog_ex("Incomplete write to ReSkate.log");
    }
    // WriteFile already handed each message to Windows. Avoid durable disk
    // flushes in game hooks; crashes perform a final best-effort flush directly.
    void flush_() override {}
    HANDLE file_{INVALID_HANDLE_VALUE};
};
class WindowConsole final : public spdlog::sinks::base_sink<std::mutex> {
public:
    WindowConsole() {
        handle_ = CreateFileW(L"CONOUT$", GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE && AllocConsole()) {
            owned_ = true;
            SetConsoleTitleW(L"ReSkate Console");
            // Text selection in a classic console must not pause game hooks.
            const auto input = GetStdHandle(STD_INPUT_HANDLE);
            DWORD mode{};
            if (GetConsoleMode(input, &mode))
                SetConsoleMode(input, (mode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
            handle_ = CreateFileW(L"CONOUT$", GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr);
        }
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (handle_ != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle_, &info)) original_ = info.wAttributes;
    }
    ~WindowConsole() override {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        if (owned_) FreeConsole();
    }
    bool available() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        if (!available()) return;
        spdlog::memory_buf_t formatted;
        formatter_->format(message, formatted);
        const auto count = MultiByteToWideChar(CP_UTF8, 0, formatted.data(), static_cast<int>(formatted.size()), nullptr, 0);
        if (count <= 0) return;
        std::wstring wide(static_cast<std::size_t>(count), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, formatted.data(), static_cast<int>(formatted.size()), wide.data(), count);
        const WORD color = message.level >= spdlog::level::err ? FOREGROUND_RED | FOREGROUND_INTENSITY
            : message.level == spdlog::level::warn ? FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY
            : message.logger_name == "Native(S)" ? FOREGROUND_BLUE | FOREGROUND_INTENSITY
            : message.logger_name == "Native(M)" || message.logger_name == "Native(U)" ? FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY
            : message.logger_name == "Native(F)" ? FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY
            : message.logger_name == "Native(C)" ? FOREGROUND_GREEN | FOREGROUND_INTENSITY
            : message.logger_name == "Native(A)" ? FOREGROUND_RED | FOREGROUND_GREEN
            : original_;
        SetConsoleTextAttribute(handle_, color);
        DWORD written{}; WriteConsoleW(handle_, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
        SetConsoleTextAttribute(handle_, original_);
    }
    void flush_() override {}
    HANDLE handle_{INVALID_HANDLE_VALUE};
    WORD original_{FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE};
    bool owned_{};
};
struct RepeatedMessage {
    Level level{};
    Channel channel{};
    Context context{};
    std::string text;
    ULONGLONG last{};
    std::uint64_t suppressed{};
};
struct State {
    std::mutex mutex;
    std::atomic<Level> level{Level::info};
    std::atomic<bool> file_output{};
    bool initialized{}, console{};
    Options options;
    std::vector<RepeatedMessage> repeats;
    std::array<std::shared_ptr<spdlog::logger>, context_count> loggers;
    ConsoleLogBuffer history{{2048, 2 * 1024 * 1024, 8300}};
};
State& state() { static auto* value = new State; return *value; }
void sink_error(const std::string& error) noexcept {
    state().file_output.store(false);
    OutputDebugStringA("ReSkate logging sink failed: ");
    OutputDebugStringA(error.c_str());
    OutputDebugStringA("\n");
}
// State::mutex serializes history and all sinks so every view has the same order.
void emit_locked(State& s, Level level, Context context, Channel channel, std::string_view text) {
    const auto index = static_cast<std::size_t>(context);
    s.history.append(text, level, channel, context);
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        const auto line = text.substr(start, end == std::string::npos ? text.size() - start : end - start);
        const auto payload = std::format("[{}] [{}] {}", name(channel), name(level), line);
        if (s.loggers[index]) s.loggers[index]->log(native_level(level), payload);
        else OutputDebugStringA(std::format("{}: {}\n", name(context), payload).c_str());
        if (end == std::string::npos) break;
        start = end + 1;
    }
}
void report_repeats(State& s, RepeatedMessage& repeat) {
    if (!repeat.suppressed) return;
    if (enabled(repeat.level)) emit_locked(s, repeat.level, repeat.context, repeat.channel,
        std::format("Repeated {} more times: {}", repeat.suppressed, repeat.text));
    repeat.suppressed = 0;
}
}

bool initialize(const Options& options) noexcept {
    detail::PreserveError preserve;
    try {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        if (s.initialized) return s.file_output.load();
        s.options = options;
        s.level.store(options.level);
        std::vector<spdlog::sink_ptr> sinks;
        auto debugger = std::make_shared<spdlog::sinks::msvc_sink_mt>();
        sinks.push_back(debugger);
        if (options.external_console) {
            auto console = std::make_shared<WindowConsole>();
            s.console = console->available();
            if (s.console) sinks.push_back(console);
        }
        try {
            std::filesystem::create_directories(options.directory);
            sinks.push_back(std::make_shared<SharedFile>(options.directory / L"ReSkate.log", options.truncate_file));
            s.file_output.store(true);
        } catch (const std::exception& error) { sink_error(error.what()); }
        for (std::size_t i = 0; i < context_count; ++i) {
            auto logger = std::make_shared<spdlog::logger>(std::string(context_names[i]), sinks.begin(), sinks.end());
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%P:%t] %n: %v");
            logger->set_level(spdlog::level::trace);
            // Sinks write synchronously without a logging worker. Startup messages
            // remain readable while diagnosing a hang.
            logger->flush_on(spdlog::level::trace);
            logger->set_error_handler(sink_error);
            s.loggers[i] = std::move(logger);
        }
        s.initialized = true;
        detail::install_crash_report(options.directory);
        const auto reporter = crash_report::start(options.directory);
        emit_locked(s, reporter == crash_report::StartResult::failed ? Level::warning : Level::info,
            Context::sdk, Channel::runtime,
            reporter == crash_report::StartResult::ready ? "Crash reporting enabled."
            : reporter == crash_report::StartResult::disabled ? "Crash reporting disabled."
            : "Crash reporter could not start; local crash logging remains active.");
        if (trace::start())
            emit_locked(s, Level::info, Context::sdk, Channel::runtime, "Performance timings are sent for this launch.");
        return s.file_output.load();
    } catch (...) {
        OutputDebugStringA("ReSkate logging initialization failed; debugger output remains available.\n");
        return false;
    }
}
bool enabled(Level level) noexcept {
    detail::PreserveError preserve;
    try { const auto minimum = state().level.load(); return level != Level::off && minimum != Level::off && level >= minimum; }
    catch (...) { return level >= Level::warning && level != Level::off; }
}
void set_level(Level level) noexcept { detail::PreserveError preserve; try { state().level.store(level); } catch (...) {} }
void write(Level level, Channel channel, std::string_view message) noexcept {
    write(level, default_context(channel), channel, message);
}
void write(Level level, Context context, Channel channel, std::string_view message) noexcept {
    detail::PreserveError preserve;
    try {
        if (level == Level::off || (!enabled(level) && channel != Channel::command) || message.empty()) return;
        auto text = detail::clean(message);
        if (text.empty()) return;
        auto& s = state();
        std::lock_guard lock(s.mutex);
        const auto index = static_cast<std::size_t>(channel);
        if (index >= channel_count) channel = Channel::runtime;
        if (static_cast<std::size_t>(context) >= context_count) context = Context::sdk;
        const auto now = GetTickCount64();
        for (auto& repeat : s.repeats)
            if (now - repeat.last >= 5000) report_repeats(s, repeat);
        if (channel != Channel::command && level != Level::critical) {
            auto found = std::find_if(s.repeats.begin(), s.repeats.end(), [&](const auto& entry) {
                return entry.level == level && entry.channel == channel && entry.context == context && entry.text == text;
            });
            if (found != s.repeats.end()) {
                if (now - found->last < 5000) { ++found->suppressed; return; }
                found->last = now;
            } else {
                if (s.repeats.size() == 128) {
                    auto oldest = std::min_element(s.repeats.begin(), s.repeats.end(),
                        [](const auto& left, const auto& right) { return left.last < right.last; });
                    report_repeats(s, *oldest);
                    *oldest = {level, channel, context, text, now, 0};
                } else s.repeats.push_back({level, channel, context, text, now, 0});
            }
        }
        emit_locked(s, level, context, channel, text);
    } catch (...) { OutputDebugStringA("ReSkate: a log message could not be recorded.\n"); }
}
void write(Level level, Channel channel, std::wstring_view message) noexcept {
    write(level, default_context(channel), channel, message);
}
void write(Level level, Context context, Channel channel, std::wstring_view message) noexcept {
    detail::PreserveError preserve;
    try { write(level, context, channel, detail::utf8(message)); } catch (...) {}
}
void vprintf(Level level, Channel channel, const char* format, va_list args) noexcept {
    vprintf(level, default_context(channel), channel, format, args);
}
void vprintf(Level level, Context context, Channel channel, const char* format, va_list args) noexcept {
    detail::PreserveError preserve;
    if (!format || level == Level::off || (!enabled(level) && channel != Channel::command)) return;
    std::array<char, 8192> message{};
    _vsnprintf_s(message.data(), message.size(), _TRUNCATE, format, args);
    write(level, context, channel, message.data());
}
void printf(Level level, Channel channel, const char* format, ...) noexcept {
    detail::PreserveError preserve;
    va_list args; va_start(args, format); vprintf(level, channel, format, args); va_end(args);
}
void printf(Level level, Context context, Channel channel, const char* format, ...) noexcept {
    detail::PreserveError preserve;
    va_list args; va_start(args, format); vprintf(level, context, channel, format, args); va_end(args);
}
Status status() {
    detail::PreserveError preserve;
    auto& s = state(); std::lock_guard lock(s.mutex);
    return {s.options.directory, s.level.load(), s.initialized, s.file_output.load(), s.console};
}
std::vector<ConsoleLogLine> snapshot_after(std::uint64_t sequence) { return state().history.snapshot_after(sequence); }
std::uint64_t latest_sequence() noexcept { try { return state().history.latest_sequence(); } catch (...) { return 0; } }
void flush() noexcept {
    detail::PreserveError preserve;
    try {
        auto& s = state(); std::lock_guard lock(s.mutex);
        for (auto& repeat : s.repeats) report_repeats(s, repeat);
        for (const auto& logger : s.loggers) if (logger) logger->flush();
    } catch (...) {}
}
void shutdown() noexcept {
    detail::PreserveError preserve;
    flush();
    try {
        auto& s = state(); std::lock_guard lock(s.mutex);
        detail::remove_crash_report();
        for (auto& logger : s.loggers) logger.reset();
        s.repeats.clear();
        s.initialized = false; s.file_output = false; s.console = false;
    } catch (...) {}
}
}
