#include "Extension/Boot/local_settings_path.h"
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace paths = dingosdk::local_settings_path;
unsigned checks{}, groups{}, cleanup_failures{};
const char* group = "startup";

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(std::string(group) + ": " + message);
}
void begin(const char* name) { group = name; ++groups; }
std::runtime_error windows_error(const char* operation, DWORD error) {
    return std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(error) + ")");
}
std::string utf8(const fs::path& path) {
    const auto& wide = path.native();
    const auto length = static_cast<int>(wide.size());
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        wide.data(), length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) throw windows_error("Cannot encode fixture path", GetLastError());
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length,
        result.data(), bytes, nullptr, nullptr) != bytes)
        throw windows_error("Cannot encode fixture path", GetLastError());
    return result;
}
void status(std::string_view value, const fs::path& root, paths::Status expected, const char* message) {
    const auto actual = paths::validate(value, root);
    ++checks;
    if (actual != expected)
        throw std::runtime_error(std::string(group) + ": " + message + ": expected " +
            paths::status_name(expected) + ", got " + paths::status_name(actual));
}
void status(const fs::path& value, const fs::path& root, paths::Status expected, const char* message) {
    status(std::string_view(utf8(value)), root, expected, message);
}

// Every entry is created exclusively. Cleanup removes only recorded files and
// empty directories or junction links; it never walks a supplied directory.
class Fixture {
public:
    explicit Fixture(const fs::path& requested) {
        if (!requested.is_absolute()) throw std::runtime_error("Fixture parent must be absolute");
        parent_ = requested.lexically_normal();
        if (CreateDirectoryW(parent_.c_str(), nullptr)) parent_created_ = true;
        else if (GetLastError() != ERROR_ALREADY_EXISTS)
            throw windows_error("Cannot create fixture parent", GetLastError());
        const auto attributes = GetFileAttributesW(parent_.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("Fixture parent is not a directory");
        parent_ = fs::canonical(parent_);
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            const auto name = L"local-settings-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt);
            const auto candidate = parent_ / name;
            if (CreateDirectoryW(candidate.c_str(), nullptr)) { root_ = candidate; return; }
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                throw windows_error("Cannot create exclusive fixture directory", GetLastError());
        }
        throw std::runtime_error("Cannot reserve an exclusive fixture directory");
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        for (auto it = files_.rbegin(); it != files_.rend(); ++it) {
            if (!DeleteFileW(it->c_str())) cleanup_error("remove fixture file", GetLastError());
        }
        for (auto it = directories_.rbegin(); it != directories_.rend(); ++it) {
            if (!RemoveDirectoryW(it->c_str())) cleanup_error("remove empty fixture directory or link", GetLastError());
        }
        if (!root_.empty() && !RemoveDirectoryW(root_.c_str())) cleanup_error("remove exclusive fixture directory", GetLastError());
        if (parent_created_ && !RemoveDirectoryW(parent_.c_str())) cleanup_error("remove empty fixture parent", GetLastError());
    }
    const fs::path& root() const noexcept { return root_; }
    fs::path directory(const fs::path& path) {
        owned(path);
        if (!CreateDirectoryW(path.c_str(), nullptr)) throw windows_error("Cannot create exclusive fixture child", GetLastError());
        directories_.push_back(path);
        return path;
    }
    fs::path file(const fs::path& path, std::string_view content) {
        owned(path);
        const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw windows_error("Cannot create exclusive fixture file", GetLastError());
        files_.push_back(path);
        DWORD written{};
        const bool ok = WriteFile(handle, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) != FALSE;
        const auto error = GetLastError();
        CloseHandle(handle);
        if (!ok || written != content.size()) throw windows_error("Cannot write fixture canary", error);
        return path;
    }
    void unlink(const fs::path& path) {
        const auto found = std::find(directories_.begin(), directories_.end(), path);
        if (found == directories_.end()) throw std::runtime_error("Attempted cleanup of an unowned directory");
        if (!RemoveDirectoryW(path.c_str())) throw windows_error("Cannot remove junction link", GetLastError());
        directories_.erase(found);
    }
private:
    void owned(const fs::path& path) const {
        if (path == root_ || !dingosdk::user_data::detail::within(path.lexically_normal(), root_))
            throw std::runtime_error("Fixture child escaped its exclusive directory");
    }
    static void cleanup_error(const char* operation, DWORD error) noexcept {
        ++cleanup_failures;
        std::fprintf(stderr, "Fixture cleanup: %s failed (Windows error %lu); no recursive cleanup attempted\n", operation, error);
    }
    fs::path parent_, root_;
    bool parent_created_{};
    std::vector<fs::path> files_, directories_;
};

std::string read_canary(const fs::path& path) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw windows_error("Cannot read fixture canary", GetLastError());
    std::array<char, 64> buffer{};
    DWORD count{};
    const bool ok = ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) != FALSE;
    const auto error = GetLastError();
    CloseHandle(handle);
    if (!ok) throw windows_error("Cannot read fixture canary", error);
    return std::string(buffer.data(), count);
}

void encoding_and_absolute_paths(const fs::path& selected) {
    begin("strict UTF-8 and absolute paths");
    const std::array<std::string, 8> invalid{
        std::string{}, std::string("inside\0outside", 14), std::string("\x80", 1),
        std::string("\xc0\xaf", 2), std::string("\xe2\x82", 2), std::string("\xed\xa0\x80", 3),
        std::string("\xf4\x90\x80\x80", 4), std::string("\xff", 1)};
    for (const auto& value : invalid)
        status(std::string_view(value), selected, paths::Status::invalid_encoding, "malformed encoding must be refused before path interpretation");
    for (const auto* value : {L"settings.json", L".\\settings.json", L"..\\settings.json", L"\\settings.json", L" "})
        status(fs::path(value), selected, paths::Status::relative, "a non-absolute native path must be refused");
    status(fs::path(selected.root_name().wstring() + L"settings.json"), selected,
        paths::Status::relative, "a drive-relative path must not inherit the process directory");
    status(selected / L"settings.json", {}, paths::Status::unresolved, "an absent selected root cannot admit a path");
    status(selected / L"settings.json", L"relative-root", paths::Status::unresolved, "a relative selected root cannot admit a path");
}

void component_boundaries(Fixture& fixture, const fs::path& selected) {
    begin("component boundaries");
    const auto child = fixture.directory(selected / L"child");
    const auto sibling = fixture.directory(fixture.root() / L"selected-other");
    const auto outside = fixture.directory(fixture.root() / L"outside");
    status(selected, selected, paths::Status::accepted, "the exact selected root is within itself");
    status(selected / L"settings.json", selected, paths::Status::accepted, "a missing final file beneath the root is allowed");
    status(child / L".." / L"settings.json", selected, paths::Status::accepted, "an internal parent component resolves inside");
    status(selected / L".." / L"outside" / L"settings.json", selected, paths::Status::outside_selection, "a parent escape is outside");
    status(sibling / L"settings.json", selected, paths::Status::outside_selection, "a sibling sharing the root's text prefix is outside");
    status(outside / L"settings.json", selected, paths::Status::outside_selection, "an existing other directory is outside");
    status(selected.parent_path(), selected, paths::Status::outside_selection, "an ancestor is outside");
    status(selected.root_path() / L"unrelated-settings.json", selected, paths::Status::outside_selection, "a drive-root sibling is outside");
}

void canonical_unicode_and_case(Fixture& fixture, const fs::path& selected) {
    begin("Windows canonical paths and Unicode");
    const auto unicode = fixture.directory(selected / L"profile-\u00e9-\u65e5\u672c-\U0001f6f9");
    const auto existing = fixture.file(unicode / L"settings-\u00e9.json", "unicode-canary");
    status(existing, selected, paths::Status::accepted, "valid two-, three- and four-byte Unicode reaches the same Windows file");
    status(unicode / L"new" / L"settings.json", selected, paths::Status::accepted, "a missing Unicode descendant remains inside");
    status(fs::path(existing.generic_wstring()), selected, paths::Status::accepted, "forward separators resolve to the same Windows destination");
    status(fixture.root() / L"SELECTED" / unicode.filename() / existing.filename(), selected,
        paths::Status::accepted, "ASCII component case follows Windows case-insensitive comparison");
    status(selected / L"child" / L"." / L".." / unicode.filename() / existing.filename(), selected,
        paths::Status::accepted, "canonical normalization retains the selected destination");
    check(read_canary(existing) == "unicode-canary", "path validation changed an existing selected-root file");
}

class JunctionUnavailable : public std::runtime_error {
public:
    explicit JunctionUnavailable(const std::string& message) : std::runtime_error(message) {}
};
bool unsupported_junction_error(DWORD error) noexcept {
    return error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED ||
        error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD;
}
void make_junction(Fixture& fixture, const fs::path& link, const fs::path& target) {
    std::array<wchar_t, 32768> volume{};
    if (!GetVolumePathNameW(fixture.root().c_str(), volume.data(), static_cast<DWORD>(volume.size())))
        throw windows_error("Cannot query fixture volume", GetLastError());
    DWORD flags{};
    if (!GetVolumeInformationW(volume.data(), nullptr, 0, nullptr, nullptr, &flags, nullptr, 0))
        throw windows_error("Cannot query fixture filesystem", GetLastError());
    if (!(flags & FILE_SUPPORTS_REPARSE_POINTS))
        throw JunctionUnavailable("Fixture filesystem does not support reparse points");
    const auto destination = fs::canonical(target).wstring();
    const auto substitute = destination.starts_with(L"\\\\?\\") ? L"\\??\\" + destination.substr(4)
        : destination.starts_with(L"\\\\") ? L"\\??\\UNC\\" + destination.substr(2) : L"\\??\\" + destination;
    struct Buffer {
        DWORD tag{IO_REPARSE_TAG_MOUNT_POINT};
        USHORT length{}, reserved{}, substitute_offset{}, substitute_length{}, print_offset{}, print_length{};
        wchar_t paths[4096]{};
    } buffer;
    static_assert(offsetof(Buffer, paths) == 16);
    const auto characters = substitute.size() + 1 + destination.size() + 1;
    if (characters > std::size(buffer.paths)) throw std::runtime_error("Fixture junction target exceeds its bounded buffer");
    buffer.substitute_length = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    buffer.print_offset = static_cast<USHORT>((substitute.size() + 1) * sizeof(wchar_t));
    buffer.print_length = static_cast<USHORT>(destination.size() * sizeof(wchar_t));
    const auto path_bytes = characters * sizeof(wchar_t);
    buffer.length = static_cast<USHORT>(4 * sizeof(USHORT) + path_bytes);
    std::copy(substitute.begin(), substitute.end(), buffer.paths);
    std::copy(destination.begin(), destination.end(), buffer.paths + substitute.size() + 1);
    fixture.directory(link);
    const auto handle = CreateFileW(link.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (unsupported_junction_error(error)) throw JunctionUnavailable(windows_error("Junction open unavailable", error).what());
        throw windows_error("Cannot open fixture junction", error);
    }
    DWORD returned{};
    const bool made = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer,
        static_cast<DWORD>(offsetof(Buffer, paths) + path_bytes), nullptr, 0, &returned, nullptr) != FALSE;
    const auto error = GetLastError();
    FILE_ATTRIBUTE_TAG_INFO information{};
    const bool inspected = made && GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &information,
        static_cast<DWORD>(sizeof(information))) != FALSE;
    CloseHandle(handle);
    if (!made) {
        if (unsupported_junction_error(error)) throw JunctionUnavailable(windows_error("Junction creation unavailable", error).what());
        throw windows_error("Cannot create fixture junction", error);
    }
    check(inspected && information.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT &&
        (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "fixture must be a real directory junction, not a symbolic link");
}

void junction_boundaries(Fixture& fixture, const fs::path& selected) {
    begin("real directory-junction destinations");
    const auto outside = fixture.root() / L"outside";
    const auto inside = fixture.directory(selected / L"inside-target");
    const auto outside_file = fixture.file(outside / L"canary.json", "outside-canary");
    const auto inside_file = fixture.file(inside / L"canary.json", "inside-canary");
    const auto escape = selected / L"outside-link";
    const auto within = selected / L"inside-link";
    make_junction(fixture, escape, outside);
    make_junction(fixture, within, inside);
    status(escape / outside_file.filename(), selected, paths::Status::outside_selection, "a lexically contained junction to an outside directory is refused");
    status(escape / L"missing" / L"settings.json", selected, paths::Status::outside_selection, "a missing suffix cannot hide a junction escape");
    status(within / inside_file.filename(), selected, paths::Status::accepted, "a junction whose real destination stays inside is accepted");
    status(within / L"missing" / L"settings.json", selected, paths::Status::accepted, "a missing suffix beneath an internal junction stays inside");
    check(read_canary(outside_file) == "outside-canary" && read_canary(inside_file) == "inside-canary", "junction validation changed a target file");
    fixture.unlink(escape);
    fixture.unlink(within);
    check(read_canary(outside_file) == "outside-canary" && read_canary(inside_file) == "inside-canary", "removing junction links touched their targets");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 3 || (argc == 3 && std::wstring_view(argv[2]) != L"--junction")) {
        std::fprintf(stderr, "Usage: local_settings_path_tests <absolute fixture parent> [--junction]\n");
        return 2;
    }
    int result{};
    try {
        {
            Fixture fixture{fs::path(argv[1])};
            const auto selected = fixture.directory(fixture.root() / L"selected");
            encoding_and_absolute_paths(selected);
            component_boundaries(fixture, selected);
            canonical_unicode_and_case(fixture, selected);
            if (argc == 3) junction_boundaries(fixture, selected);
        }
    } catch (const JunctionUnavailable& error) {
        std::printf("SKIP: directory junction unavailable: %s\n", error.what());
        result = 77;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "local settings path failed: %s\n", error.what());
        result = 1;
    }
    if (cleanup_failures) result = 1;
    std::printf("local settings path: %u groups, %u checks, %u cleanup failures\n", groups, checks, cleanup_failures);
    return result;
}
