#include "console_core.h"
#include "Engine/Core/Text/ascii.h"

#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace dingosdk {
namespace {

bool is_separator(char value) noexcept {
    return value == ' ' || value == '\t';
}

bool is_allowed_input_character(unsigned char value) noexcept {
    return value == static_cast<unsigned char>('\t') ||
           (value >= static_cast<unsigned char>(' ') &&
            value <= static_cast<unsigned char>('~'));
}

} // namespace

CommandParseResult parse_console_command(std::string_view input) {
    CommandParseResult result;
    if (input.size() > console_max_command_bytes) {
        result.error = CommandParseError::input_too_long;
        result.error_offset = console_max_command_bytes;
        return result;
    }

    for (std::size_t index = 0; index < input.size(); ++index) {
        if (!is_allowed_input_character(static_cast<unsigned char>(input[index]))) {
            result.error = CommandParseError::invalid_character;
            result.error_offset = index;
            return result;
        }
    }

    std::string argument;
    argument.reserve(input.size());
    bool argument_started = false;
    bool escaped = false;
    char quote = '\0';

    const auto finish_argument = [&]() -> bool {
        if (!argument_started) return true;
        if (result.arguments.size() == console_max_arguments) {
            result.error = CommandParseError::too_many_arguments;
            return false;
        }
        result.arguments.push_back(argument);
        argument.clear();
        argument_started = false;
        return true;
    };

    for (std::size_t index = 0; index < input.size(); ++index) {
        const char value = input[index];
        if (escaped) {
            argument.push_back(value);
            argument_started = true;
            escaped = false;
            continue;
        }
        if (value == '\\') {
            escaped = true;
            argument_started = true;
            continue;
        }
        if (quote != '\0') {
            if (value == quote) {
                quote = '\0';
            } else {
                argument.push_back(value);
            }
            argument_started = true;
            continue;
        }
        if (value == '\'' || value == '"') {
            quote = value;
            argument_started = true;
            continue;
        }
        if (is_separator(value)) {
            if (!finish_argument()) {
                result.error_offset = index;
                result.arguments.clear();
                return result;
            }
            continue;
        }
        argument.push_back(value);
        argument_started = true;
    }

    if (escaped) {
        result.error = CommandParseError::dangling_escape;
        result.error_offset = input.empty() ? 0 : input.size() - 1;
        result.arguments.clear();
        return result;
    }
    if (quote != '\0') {
        result.error = CommandParseError::unterminated_quote;
        result.error_offset = input.size();
        result.arguments.clear();
        return result;
    }
    if (!finish_argument()) {
        result.error_offset = input.size();
        result.arguments.clear();
    }
    return result;
}

bool ascii_case_insensitive_starts_with(std::string_view value,
                                        std::string_view prefix) noexcept {
    if (prefix.size() > value.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (ascii_lower(value[index]) != ascii_lower(prefix[index])) return false;
    }
    return true;
}

ConsoleLogBuffer::ConsoleLogBuffer(ConsoleLogLimits limits)
    : limits_(limits) {
    if (limits_.max_lines == 0 || limits_.max_bytes == 0 ||
        limits_.max_line_bytes == 0 || limits_.max_line_bytes > limits_.max_bytes) {
        throw std::invalid_argument("Invalid console log limits");
    }
}

std::uint64_t console_timestamp_ms() noexcept {
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count());
}

const char* console_source_name(ConsoleSource source) noexcept {
    return logging::name(source).data();
}

std::string format_console_line(const ConsoleLogLine& line, bool timestamp) {
    std::ostringstream output;
    if (timestamp) output << '[' << std::fixed << std::setprecision(3)
        << std::setw(9) << static_cast<double>(line.elapsed_ms) / 1000.0 << "] ";
    output << logging::name(line.context) << ": [" << console_source_name(line.source)
        << "] [" << logging::name(line.severity) << "] ";
    output << line.text;
    return output.str();
}

std::uint64_t ConsoleLogBuffer::append(std::string_view text,
    ConsoleSeverity severity, ConsoleSource source, logging::Context context) {
    std::lock_guard lock(mutex_);
    if (text.empty()) return latest_sequence_;
    const auto elapsed_ms = console_timestamp_ms();

    std::size_t start = 0;
    while (start < text.size()) {
        const auto newline = text.find('\n', start);
        if (newline == std::string_view::npos) {
            append_line_locked(text.substr(start), elapsed_ms, severity, source, context);
            break;
        }
        auto line = text.substr(start, newline - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        append_line_locked(line, elapsed_ms, severity, source, context);
        start = newline + 1;
    }
    return latest_sequence_;
}

std::vector<ConsoleLogLine> ConsoleLogBuffer::snapshot_after(
    std::uint64_t after_sequence) const {
    std::lock_guard lock(mutex_);
    std::vector<ConsoleLogLine> snapshot;
    for (const auto& line : lines_) {
        if (line.sequence > after_sequence) snapshot.push_back(line);
    }
    return snapshot;
}

std::uint64_t ConsoleLogBuffer::latest_sequence() const noexcept {
    std::lock_guard lock(mutex_);
    return latest_sequence_;
}

void ConsoleLogBuffer::append_line_locked(std::string_view text, std::uint64_t elapsed_ms,
    ConsoleSeverity severity, ConsoleSource source, logging::Context context) {
    if (latest_sequence_ == (std::numeric_limits<std::uint64_t>::max)()) {
        throw std::overflow_error("Console log sequence exhausted");
    }
    const auto bounded = text.substr(0, limits_.max_line_bytes);
    lines_.push_back({++latest_sequence_, std::string(bounded), elapsed_ms, severity, source, context});
    stored_bytes_ += bounded.size();
    enforce_limits_locked();
}

void ConsoleLogBuffer::enforce_limits_locked() {
    while (lines_.size() > limits_.max_lines || stored_bytes_ > limits_.max_bytes) {
        stored_bytes_ -= lines_.front().text.size();
        lines_.pop_front();
    }
}

} // namespace dingosdk
