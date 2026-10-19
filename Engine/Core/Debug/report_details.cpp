#include <Windows.h>
#include <bcrypt.h>
#include <DbgHelp.h>
#include "report_details.h"
#include "report_envelope.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <span>
#include <vector>

namespace dingosdk::crash_report {
namespace {
constexpr std::size_t max_breadcrumb_text = 600;
// How much of the log's end is read for breadcrumbs: far more than 100 lines ever take.
constexpr std::uintmax_t breadcrumb_window = 1024 * 1024;

// JSON text must be UTF-8: anything else in a line becomes '?'.
std::string valid_utf8(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t length = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        bool ok = length && i + length <= text.size() && !(length == 2 && c < 0xc2);
        for (std::size_t k = 1; ok && k < length; ++k) ok = (static_cast<unsigned char>(text[i + k]) & 0xc0) == 0x80;
        if (!ok) { out.push_back('?'); ++i; continue; }
        if (length == 1 && c < 0x20 && c != '\t') out.push_back(' ');
        else out.append(text.substr(i, length));
        i += length;
    }
    return out;
}
// "[2026-10-16 16:56:20.354]" in local time -> seconds since 1970 (UTC), or a negative number.
double log_time(std::string_view stamp) {
    int y{}, mo{}, d{}, h{}, mi{}, s{}, ms{};
    if (stamp.size() < 25 || sscanf_s(std::string(stamp.substr(1, 23)).c_str(), "%d-%d-%d %d:%d:%d.%d", &y, &mo, &d, &h, &mi, &s, &ms) != 7)
        return -1.0;
    SYSTEMTIME local{static_cast<WORD>(y), static_cast<WORD>(mo), 0, static_cast<WORD>(d), static_cast<WORD>(h), static_cast<WORD>(mi),
        static_cast<WORD>(s), static_cast<WORD>(ms)};
    SYSTEMTIME utc{};
    FILETIME file{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &file)) return -1.0;
    ULARGE_INTEGER ticks{};
    ticks.LowPart = file.dwLowDateTime;
    ticks.HighPart = file.dwHighDateTime;
    return static_cast<double>(ticks.QuadPart) / 1e7 - 11644473600.0;
}
// One log line: "[time] [pid:tid] Source: [Channel] [LEVEL] text".
struct Line { double time{-1}; std::string category, level, text; bool debug{}; };
Line parse_line(std::string_view raw) {
    Line line;
    line.level = "info";
    line.time = log_time(raw);
    auto rest = raw;
    if (line.time >= 0.0) {
        if (const auto close = rest.find("] [", 1); close != std::string_view::npos) rest.remove_prefix(close + 2);
        if (const auto close = rest.find("] "); close != std::string_view::npos) rest.remove_prefix(close + 2);
    }
    std::string source;
    if (const auto colon = rest.find(": ["); colon != std::string_view::npos && colon < 24) {
        source = rest.substr(0, colon);
        rest.remove_prefix(colon + 2);
    }
    std::string channel;
    if (rest.starts_with('[')) {
        if (const auto close = rest.find("] "); close != std::string_view::npos && close < 40) {
            channel = rest.substr(1, close - 1);
            rest.remove_prefix(close + 2);
        }
    }
    if (rest.starts_with('[')) {
        if (const auto close = rest.find("] "); close != std::string_view::npos && close < 12) {
            const auto level = rest.substr(1, close - 1);
            rest.remove_prefix(close + 2);
            line.debug = level == "DEBUG" || level == "TRACE";
            line.level = level == "WARNING" ? "warning" : level == "ERROR" ? "error" : level == "FATAL" || level == "CRITICAL" ? "fatal"
                       : line.debug ? "debug" : "info";
        }
    }
    line.category = source.empty() ? channel : channel.empty() ? source : source + "/" + channel;
    line.text = valid_utf8(rest.substr(0, max_breadcrumb_text));
    if (rest.size() > max_breadcrumb_text) line.text += "...";
    line.category = valid_utf8(line.category);
    return line;
}
}

std::filesystem::path event_details_path(const std::filesystem::path& dump) { return dump.wstring() + L".event.json"; }

Json log_breadcrumbs(const std::filesystem::path& log, std::size_t count, std::size_t recent) noexcept {
    try {
        std::ifstream input(log, std::ios::binary);
        if (!input) return {};
        input.seekg(0, std::ios::end);
        const auto size = static_cast<std::uintmax_t>(input.tellg());
        const auto start = size > breadcrumb_window ? size - breadcrumb_window : 0;
        input.seekg(static_cast<std::streamoff>(start));
        std::string text(static_cast<std::size_t>(size - start), '\0');
        if (!input.read(text.data(), static_cast<std::streamsize>(text.size()))) return {};
        std::vector<std::string_view> lines;
        for (std::size_t at = 0; at < text.size();) {
            auto end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            auto line = std::string_view(text).substr(at, end - at);
            if (line.ends_with('\r')) line.remove_suffix(1);
            if (!line.empty()) lines.push_back(line);
            at = end + 1;
        }
        if (start && !lines.empty()) lines.erase(lines.begin()); // (the first one is cut short)
        std::vector<Line> kept;
        for (std::size_t back = 0; back < lines.size() && kept.size() < count; ++back) {
            auto line = parse_line(lines[lines.size() - 1 - back]);
            if (line.debug && back >= recent) continue;
            kept.push_back(std::move(line));
        }
        Json values = Json::array();
        double last_time = -1.0;
        for (auto it = kept.rbegin(); it != kept.rend(); ++it) {
            Json crumb = Json::object();
            // (A line without a time of its own, a continuation, takes the one before it.)
            if (it->time >= 0.0) last_time = it->time;
            if (last_time >= 0.0) crumb["timestamp"] = last_time;
            if (!it->category.empty()) crumb["category"] = it->category;
            crumb["level"] = it->level;
            crumb["message"] = it->text;
            values.push_back(std::move(crumb));
        }
        Json result = Json::object();
        result["values"] = std::move(values);
        return result;
    } catch (...) { return {}; }
}

std::string crash_site(const std::filesystem::path& dump) noexcept {
    try {
        std::ifstream input(dump, std::ios::binary);
        if (!input) return {};
        std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const std::span data(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
        const auto read = [&]<class T>(std::size_t offset, T& value) {
            if (offset > data.size() || sizeof(T) > data.size() - offset) return false;
            std::memcpy(&value, data.data() + offset, sizeof(T));
            return true;
        };
        MINIDUMP_HEADER header{};
        if (!read(std::size_t{0}, header) || header.Signature != MINIDUMP_SIGNATURE || header.NumberOfStreams > 256) return {};
        MINIDUMP_EXCEPTION_STREAM exception{};
        MINIDUMP_LOCATION_DESCRIPTOR modules{};
        bool have_exception{}, have_modules{};
        for (ULONG32 i = 0; i < header.NumberOfStreams; ++i) {
            MINIDUMP_DIRECTORY entry{};
            if (!read(static_cast<std::size_t>(header.StreamDirectoryRva) + i * sizeof(entry), entry)) return {};
            if (entry.StreamType == ExceptionStream) have_exception = read(static_cast<std::size_t>(entry.Location.Rva), exception);
            if (entry.StreamType == ModuleListStream) { modules = entry.Location; have_modules = true; }
        }
        if (!have_exception || !have_modules) return {};
        const auto address = exception.ExceptionRecord.ExceptionAddress;
        ULONG32 module_count{};
        if (!read(static_cast<std::size_t>(modules.Rva), module_count) || module_count > 4096) return {};
        for (ULONG32 i = 0; i < module_count; ++i) {
            MINIDUMP_MODULE module{};
            if (!read(static_cast<std::size_t>(modules.Rva) + sizeof(ULONG32) + i * sizeof(MINIDUMP_MODULE), module)) return {};
            if (address < module.BaseOfImage || address - module.BaseOfImage >= module.SizeOfImage) continue;
            ULONG32 name_bytes{};
            if (!read(static_cast<std::size_t>(module.ModuleNameRva), name_bytes) || name_bytes > 2048 || name_bytes % 2) return {};
            const auto name_at = static_cast<std::size_t>(module.ModuleNameRva) + sizeof(ULONG32);
            if (name_at > data.size() || name_bytes > data.size() - name_at) return {};
            std::wstring name(name_bytes / 2, L'\0');
            std::memcpy(name.data(), data.data() + name_at, name_bytes);
            const auto file = std::filesystem::path(name).filename().wstring();
            std::string ascii;
            for (const auto c : file) ascii.push_back(c < 0x80 && c >= 0x20 ? static_cast<char>(c) : '?');
            return std::format("{}+{:#x}", ascii, address - module.BaseOfImage);
        }
        return {};
    } catch (...) { return {}; }
}

std::string install_id(const std::filesystem::path& directory) noexcept {
    try {
        const auto path = directory / L"install-id.txt";
        {
            std::ifstream input(path, std::ios::binary);
            std::string id;
            if (input && std::getline(input, id) && id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos)
                return id;
        }
        std::array<unsigned char, 16> random{};
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            return {};
        std::string id;
        for (const auto byte : random) id += std::format("{:02x}", byte);
        std::filesystem::create_directories(directory);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << id << '\n';
        return output ? id : std::string{};
    } catch (...) { return {}; }
}

void remove_report_files(const std::filesystem::path& dump) noexcept {
    std::error_code error;
    std::filesystem::remove(dump, error);
    if (error) return;
    std::filesystem::remove(dump.wstring() + L".json", error);
    std::filesystem::remove(log_attachment_path(dump), error);
    std::filesystem::remove(event_details_path(dump), error);
}

void keep_sent_report(const std::filesystem::path& dump, std::size_t keep) noexcept {
    try {
        const auto sent = dump.parent_path() / L"sent";
        std::error_code error;
        std::filesystem::create_directories(sent, error);
        // The dump last: a report in sent/ without it is pruned, never one in the queue twice.
        for (const auto& file : {std::filesystem::path(dump.wstring() + L".json"), log_attachment_path(dump), event_details_path(dump), dump}) {
            if (!std::filesystem::exists(file, error)) continue;
            std::filesystem::rename(file, sent / file.filename(), error);
            if (error) std::filesystem::remove(file, error);
        }
        std::vector<std::filesystem::directory_entry> kept;
        for (const auto& entry : std::filesystem::directory_iterator(sent, error))
            if (entry.is_regular_file() && entry.path().extension() == L".dmp") kept.push_back(entry);
        std::sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) { return a.last_write_time() > b.last_write_time(); });
        for (std::size_t i = keep; i < kept.size(); ++i) remove_report_files(kept[i].path());
    } catch (...) { remove_report_files(dump); }
}
}
