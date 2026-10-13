#include "native_dump.h"
#include "protocol.h"
#include "upload.h"
#include <DbgHelp.h>
#include <cstring>
#include <span>

namespace dingosdk::crash_report {
namespace {
template<class T> bool read(std::span<const std::byte> bytes, std::size_t offset, T& value) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) return false;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return true;
}
std::optional<NativeDump> inspect(const std::filesystem::path& path, DWORD process_id, DWORD process_created) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &size) ||
        size.QuadPart < sizeof(MINIDUMP_HEADER) || static_cast<std::uintmax_t>(size.QuadPart) > max_dump_bytes) return {};
    Handle mapping(CreateFileMappingW(file.value, nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!mapping.value) return {};
    auto* data = static_cast<const std::byte*>(MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, 0));
    if (!data) return {};
    struct View { const void* data; ~View() { UnmapViewOfFile(data); } } view{data};
    const std::span bytes(data, static_cast<std::size_t>(size.QuadPart));
    MINIDUMP_HEADER header{};
    if (!read(bytes, 0, header) || header.Signature != MINIDUMP_SIGNATURE ||
        (header.Version & 0xffff) != MINIDUMP_VERSION || header.NumberOfStreams > 128 ||
        header.TimeDateStamp < process_created) return {};
    MINIDUMP_MISC_INFO misc{};
    MINIDUMP_EXCEPTION_STREAM exception{};
    bool have_misc{}, have_exception{};
    for (DWORD i = 0; i < header.NumberOfStreams; ++i) {
        MINIDUMP_DIRECTORY entry{};
        if (!read(bytes, static_cast<std::size_t>(header.StreamDirectoryRva) + i * sizeof(entry), entry) ||
            entry.Location.Rva > bytes.size() || entry.Location.DataSize > bytes.size() - entry.Location.Rva) return {};
        if (entry.StreamType == MiscInfoStream && entry.Location.DataSize >= sizeof(misc))
            have_misc = read(bytes, entry.Location.Rva, misc);
        if (entry.StreamType == ExceptionStream && entry.Location.DataSize >= sizeof(exception))
            have_exception = read(bytes, entry.Location.Rva, exception);
    }
    constexpr DWORD required = MINIDUMP_MISC1_PROCESS_ID | MINIDUMP_MISC1_PROCESS_TIMES;
    if (!have_misc || !have_exception || (misc.Flags1 & required) != required || misc.ProcessId != process_id ||
        misc.ProcessCreateTime != process_created || !exception.ThreadId || !exception.ExceptionRecord.ExceptionCode) return {};
    return NativeDump{path, misc.ProcessId, misc.ProcessCreateTime, exception.ThreadId,
        exception.ExceptionRecord.ExceptionCode, header.TimeDateStamp, exception.ExceptionRecord.ExceptionAddress};
}
}
DWORD process_creation_seconds(HANDLE process) noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER time{};
    time.LowPart = created.dwLowDateTime; time.HighPart = created.dwHighDateTime;
    constexpr auto epoch = 11644473600ull;
    const auto seconds = time.QuadPart / 10000000ull;
    return seconds >= epoch ? static_cast<DWORD>(seconds - epoch) : 0;
}
std::optional<NativeDump> find_native_dump(const std::filesystem::path& directory,
        DWORD process_id, DWORD process_created) noexcept {
    try {
        if (directory.empty() || !process_id || !process_created) return {};
        std::optional<NativeDump> selected;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto extension = entry.path().extension();
            if ((extension != L".mdmp" && extension != L".dmp") || !entry.is_regular_file()) continue;
            auto candidate = inspect(entry.path(), process_id, process_created);
            if (!candidate) continue;
            if (!selected || candidate->timestamp < selected->timestamp ||
                (candidate->timestamp == selected->timestamp && extension == L".mdmp" && selected->path.extension() != L".mdmp"))
                selected = std::move(candidate);
        }
        return selected;
    } catch (...) { return {}; }
}
}
