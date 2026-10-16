#include "mod_manager.h"
#include "text_encoding.h"

#include "Engine/Vfs/mod_list.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/launcher_support.h"

#include <Windows.h>
#include <shellapi.h>

#include <miniz.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace dingosdk::launcher_mods {
namespace {

// What the game loads from a mod, and the files that only describe one.
constexpr std::array<std::string_view, 2> content_markers{"layout.toc", "reskate-levels.json"};
constexpr std::array<std::string_view, 2> info_markers{"manifest.json", "reskate-mod.json"};
// Thunderstore requires these at the top of a package (lower case here); they
// follow the mod in when it is packed one folder deeper.
constexpr std::array<std::string_view, 4> package_files{"manifest.json", "icon.png", "readme.md", "changelog.md"};
constexpr char no_mod[] = " does not contain a ReSkate mod (no layout.toc, reskate-levels.json or manifest.json).";
// Staging lives inside Mods so the final move is a rename on one volume; the
// leading dot keeps the runtime from ever treating it as a mod.
constexpr wchar_t staging_folder[] = L".reskate-install";

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return result;
}

using launcher_text::utf8;

fs::path from_utf8(std::string_view value) {
    const auto length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring output(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), length);
    return output;
}

constexpr char unsafe_link_message[] =
    "Mod folders cannot contain symbolic links, junctions, or other reparse points. Remove the link and try again.";

// FILE_ATTRIBUTE_REPARSE_POINT also catches Windows junctions and mount points.
DWORD attributes(const fs::path& path) {
    const auto value = GetFileAttributesW(path.c_str());
    if (value == INVALID_FILE_ATTRIBUTES)
        fail("Could not inspect " + utf8(path.filename().wstring()) + " (Windows error " +
             std::to_string(GetLastError()) + ").");
    return value;
}

void reject_reparse_point(const fs::path& path) {
    if (attributes(path) & FILE_ATTRIBUTE_REPARSE_POINT) fail(unsafe_link_message);
}

void verify_path_chain(const fs::path& path, bool create_missing) {
    const auto absolute = fs::absolute(path).lexically_normal();
    auto current = absolute.root_path();
    if (current.empty()) fail("Could not resolve a filesystem path.");
    for (const auto& part : absolute.relative_path()) {
        current /= part;
        DWORD value = GetFileAttributesW(current.c_str());
        if (value == INVALID_FILE_ATTRIBUTES && create_missing) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                    fail("Could not create the Mods folder (Windows error " + std::to_string(GetLastError()) + ").");
                value = GetFileAttributesW(current.c_str());
            }
        }
        if (value == INVALID_FILE_ATTRIBUTES)
            fail("Could not inspect " + utf8(current.filename().wstring()) + " (Windows error " +
                 std::to_string(GetLastError()) + ").");
        if (value & FILE_ATTRIBUTE_REPARSE_POINT) fail(unsafe_link_message);
        if (!(value & FILE_ATTRIBUTE_DIRECTORY)) fail("A folder path contains a file.");
    }
}

bool path_within(const fs::path& root, const fs::path& path) {
    const auto normalized_root = root.lexically_normal();
    const auto normalized_path = path.lexically_normal();
    auto root_part = normalized_root.begin();
    const auto root_end = normalized_root.end();
    auto path_part = normalized_path.begin();
    const auto path_end = normalized_path.end();
    for (; root_part != root_end; ++root_part, ++path_part)
        if (path_part == path_end || *root_part != *path_part) return false;
    return path_part != path_end;
}

bool safe_windows_component(std::wstring_view component) {
    if (component.empty() || component == L"." || component == L".." ||
        component.back() == L'.' || component.back() == L' ') return false;
    for (const auto ch : component)
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'|' || ch == L'?' || ch == L'*') return false;
    auto base = component.substr(0, component.find(L'.'));
    while (!base.empty() && (base.back() == L'.' || base.back() == L' ')) base.remove_suffix(1);
    std::wstring folded(base);
    for (auto& ch : folded) if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
    if (folded == L"CON" || folded == L"PRN" || folded == L"AUX" || folded == L"NUL" ||
        folded == L"CONIN$" || folded == L"CONOUT$" || folded == L"CLOCK$") return false;
    if (folded.size() == 4 && (folded.starts_with(L"COM") || folded.starts_with(L"LPT")) &&
        ((folded[3] >= L'1' && folded[3] <= L'9') || folded[3] == L'\u00b9' ||
         folded[3] == L'\u00b2' || folded[3] == L'\u00b3')) return false;
    return true;
}

std::optional<std::string> safe_relative_path(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos) return std::nullopt;
    std::string_view rest(name);
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const auto part = rest.substr(0, slash);
        if (part.empty() || !safe_windows_component(from_utf8(part).wstring())) return std::nullopt;
        if (slash == std::string_view::npos) break;
        rest.remove_prefix(slash + 1);
        if (rest.empty()) break;
    }
    return name;
}

struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = std::exchange(other.value, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

void ensure_destination_parent(const fs::path& staging, const fs::path& target);

Handle open_staging_file(const fs::path& staging, const fs::path& target) {
    ensure_destination_parent(staging, target);
    Handle output{CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    bool existing = false;
    if (output.value == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
            fail("Could not safely create a file in the staged mod.");
        output.value = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        existing = true;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (output.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(output.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
        GetFileType(output.value) != FILE_TYPE_DISK)
        fail("Could not safely create a file in the staged mod.");
    if (existing) {
        LARGE_INTEGER beginning{};
        if (!SetFilePointerEx(output.value, beginning, nullptr, FILE_BEGIN) || !SetEndOfFile(output.value))
            fail("Could not replace a file in the staged mod.");
    }
    return output;
}

void ensure_destination_parent(const fs::path& staging, const fs::path& target) {
    const auto root = fs::absolute(staging).lexically_normal();
    const auto normalized = fs::absolute(target).lexically_normal();
    if (!path_within(root, normalized)) fail("The mod contains a path outside its installation folder.");
    verify_path_chain(root, false);
    if (!(attributes(root) & FILE_ATTRIBUTE_DIRECTORY)) fail("The staging folder is not a directory.");
    auto current = root;
    const auto relative_parent = normalized.parent_path().lexically_relative(root);
    if (relative_parent == L".") return;
    for (const auto& part : relative_parent) {
        if (part == L"." || part == L".." || part.empty()) fail("The mod contains an unsafe path.");
        current /= part;
        DWORD value = GetFileAttributesW(current.c_str());
        if (value == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) ||
                !CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
                fail("Could not create a folder in the staged mod.");
            value = GetFileAttributesW(current.c_str());
        }
        if (value == INVALID_FILE_ATTRIBUTES || (value & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !(value & FILE_ATTRIBUTE_DIRECTORY)) fail(unsafe_link_message);
    }
}

void copy_to_staging(const fs::path& source, const fs::path& staging, const fs::path& target) {
    Handle input{CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
    BY_HANDLE_FILE_INFORMATION source_info{};
    if (input.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(input.value, &source_info) ||
        (source_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (source_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || GetFileType(input.value) != FILE_TYPE_DISK)
        fail(unsafe_link_message);

    auto output = open_staging_file(staging, target);

    std::array<char, 64 * 1024> buffer{};
    for (;;) {
        DWORD read{};
        if (!ReadFile(input.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            fail("Could not read a file from the mod folder.");
        if (!read) break;
        DWORD offset{};
        while (offset < read) {
            DWORD written{};
            if (!WriteFile(output.value, buffer.data() + offset, read - offset, &written, nullptr) || !written)
                fail("Could not write a file to the staged mod.");
            offset += written;
        }
    }
}

void remove_tree_without_following_links(const fs::path& path) noexcept {
    try {
        const auto absolute = fs::absolute(path).lexically_normal();
        auto current = absolute.root_path();
        for (const auto& part : absolute.relative_path()) {
            current /= part;
            const auto attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) return;
            if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (current == absolute) {
                    if (attributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(current.c_str());
                    else DeleteFileW(current.c_str());
                }
                return;
            }
        }
    } catch (...) {
        return;
    }
    const auto value = GetFileAttributesW(path.c_str());
    if (value == INVALID_FILE_ATTRIBUTES) return;
    if (value & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (value & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(path.c_str());
        else DeleteFileW(path.c_str());
        return;
    }
    if (value & FILE_ATTRIBUTE_DIRECTORY) {
        std::error_code error;
        for (fs::directory_iterator it(path, error), end; !error && it != end; it.increment(error))
            remove_tree_without_following_links(it->path());
        RemoveDirectoryW(path.c_str());
    } else {
        DeleteFileW(path.c_str());
    }
}

void validate_tree_without_links(const fs::path& root) {
    verify_path_chain(root, false);
    if (!(attributes(root) & FILE_ATTRIBUTE_DIRECTORY)) fail("The staged mod is not a folder.");
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const auto value = GetFileAttributesW(it->path().c_str());
        if (value == INVALID_FILE_ATTRIBUTES) fail("Could not inspect the staged mod.");
        if (value & FILE_ATTRIBUTE_REPARSE_POINT) fail(unsafe_link_message);
    }
    if (error) fail("Could not inspect the staged mod.");
}

// Folder names travel into engine paths (mod_list's valid_mod_name), so map
// anything else to '_' rather than refusing the mod.
std::string folder_name(std::string_view proposed) {
    std::string name;
    for (const auto value : proposed) {
        const auto ch = static_cast<unsigned char>(value);
        const bool plain = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            value == '_' || value == '-' || value == '.' || value == ' ';
        name += plain ? value : '_';
    }
    while (!name.empty() && (name.front() == '.' || name.front() == ' ')) name.erase(name.begin());
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.size() > mods::maximum_mod_name) name.resize(mods::maximum_mod_name);
    if (name.empty()) name = "Mod";
    if (!safe_windows_component(from_utf8(name).wstring())) {
        if (name.size() == mods::maximum_mod_name) name.pop_back();
        name += '_';
    }
    return name;
}

template<std::size_t N> bool one_of(const std::array<std::string_view, N>& names, std::string_view file) {
    return std::find(names.begin(), names.end(), lower(file)) != names.end();
}

std::string last_folder(const std::string& folder) {
    auto trimmed = folder;
    if (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
    const auto slash = trimmed.rfind('/');
    return slash == std::string::npos ? trimmed : trimmed.substr(slash + 1);
}

struct Root {
    std::string folder;   // "a/b/", or "" for the top
    bool content{};       // holds something the game loads
};

// The shallowest folder holding game content (layout.toc, reskate-levels.json
// or parks/<map>.park.json), else the shallowest holding only a description.
// Content wins so a package's own manifest.json at the top cannot hide a mod
// packed one folder down.
std::optional<Root> mod_root(const std::vector<std::string>& files) {
    std::optional<std::string> content, info;
    std::size_t content_depth = SIZE_MAX, info_depth = SIZE_MAX;
    for (const auto& file : files) {
        const auto slash = file.rfind('/');
        auto folder = slash == std::string::npos ? std::string() : file.substr(0, slash + 1);
        const auto leaf = slash == std::string::npos ? file : file.substr(slash + 1);
        bool is_content = one_of(content_markers, leaf);
        if (!is_content && lower(leaf).ends_with(".park.json") && lower(last_folder(folder)) == "parks") {
            folder.resize(folder.size() - std::string_view("parks/").size());
            is_content = true;
        }
        const auto depth = static_cast<std::size_t>(std::count(folder.begin(), folder.end(), '/'));
        if (is_content && depth < content_depth) { content = folder; content_depth = depth; }
        if (!is_content && one_of(info_markers, leaf) && depth < info_depth) { info = folder; info_depth = depth; }
    }
    if (content) return Root{*content, true};
    if (info) return Root{*info, false};
    return std::nullopt;
}

// A file at the top of the source that goes into a mod found deeper:
// Thunderstore's package files, when the top has a manifest.json (else they
// are not a package's).
bool carried(const Root& root, const std::vector<std::string>& files, const std::string& file) {
    if (root.folder.empty() || file.find('/') != std::string::npos || !one_of(package_files, file)) return false;
    return std::any_of(files.begin(), files.end(), [](const std::string& top) { return lower(top) == "manifest.json"; }) &&
           std::any_of(files.begin(), files.end(), [&](const std::string& top) { return lower(top) == lower(file); });
}

// Where a file of the source goes inside the mod, if anywhere. Carried package
// files replace the mod's own copies of them.
std::optional<std::string> relative_in_mod(const Root& root, const std::vector<std::string>& files, const std::string& file) {
    if (carried(root, files, file)) return file;
    if (!file.starts_with(root.folder)) return std::nullopt;
    auto relative = file.substr(root.folder.size());
    if (relative.empty() || carried(root, files, relative)) return std::nullopt;
    return relative;
}

// "Team-Map-1.2.3" (a zip downloaded from Thunderstore) installs as Team-Map,
// the folder the launcher's own Thunderstore installs use.
std::string without_version(std::string name) {
    const auto dash = name.rfind('-');
    if (dash == std::string::npos || dash == 0) return name;
    const std::string_view version(name.data() + dash + 1, name.size() - dash - 1);
    if (std::count(version.begin(), version.end(), '.') != 2 || version.empty() || version.front() == '.' ||
        version.back() == '.' || version.find("..") != std::string_view::npos ||
        version.find_first_not_of("0123456789.") != std::string_view::npos) return name;
    name.resize(dash);
    return name;
}

std::string mod_name(const Root& root, const std::string& source_name, const InstallOptions& options) {
    if (!options.folder.empty()) return folder_name(options.folder);
    return folder_name(root.folder.empty() ? without_version(source_name) : last_folder(root.folder));
}

// Last steps in staging: a source that gives the game nothing to load is
// refused when asked, and the author goes into manifest.json.
void finish(const fs::path& staging, const Root& root, const std::string& source_name, const InstallOptions& options) {
    if (options.require_content && !root.content)
        fail(source_name + " has nothing for ReSkate to load: no layout.toc, reskate-levels.json or parks folder. "
             "Only mods built with ReSkate Studio, and park mods, can be installed.");
    const auto manifest = staging / mods::manifest_file;
    const auto value = GetFileAttributesW(manifest.c_str());
    if (options.author.empty() || value == INVALID_FILE_ATTRIBUTES) return;
    if (value & FILE_ATTRIBUTE_REPARSE_POINT) fail(unsafe_link_message);
    if (value & FILE_ATTRIBUTE_DIRECTORY) return;
    try {
        std::string text;
        {
            std::ifstream input(manifest, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(input), {});
        }
        if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
        auto json = Json::parse(text, JsonLimits{1024 * 1024, 16, 65536});
        if (!json.is_object()) return;
        if (json.contains("author") && json.at("author").is_string() && !json.at("author").string().empty()) return;
        json.items()["author"] = Json(options.author);
        std::ofstream(manifest, std::ios::binary | std::ios::trunc) << json.dump(4) << '\n';
    } catch (...) {
        // An unreadable manifest is the mod's own business; the game reports it.
    }
}

// Entry names from an archive are untrusted: no absolute paths, drive
// letters, alternate streams or parent references may leave the target.
std::optional<std::string> safe_entry(std::string name) {
    auto safe = safe_relative_path(std::move(name));
    if (!safe) return std::nullopt;
    if (safe->starts_with("__MACOSX/")) return std::string();
    return safe;
}

// Package sizes come from the zip itself, so refuse one that would fill the drive.
void check_free_space(const fs::path& mods, std::uint64_t needed) {
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(mods.c_str(), &available, nullptr, nullptr)) return;
    constexpr std::uint64_t reserve = 1ull << 30;
    if (needed > available.QuadPart || available.QuadPart - needed < reserve)
        fail("This mod needs " + std::to_string(needed >> 20) + " MB, more than the drive has free.");
}

void recycle(const fs::path& path) {
    std::wstring from = path.wstring();
    from.push_back(L'\0'); // SHFileOperation takes a double-terminated list.
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = from.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT | FOF_NOERRORUI;
    if (SHFileOperationW(&operation) != 0 || operation.fAnyOperationsAborted)
        fail("Could not move " + utf8(path.filename().wstring()) + " to the Recycle Bin. Close anything using its files and try again.");
}

class Staging {
public:
    Staging(const fs::path& mods, const std::string&) : path_(mods / staging_folder) {
        const auto parent = path_;
        verify_path_chain(mods, false);
        auto parent_attributes = GetFileAttributesW(parent.c_str());
        if (parent_attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) ||
                !CreateDirectoryW(parent.c_str(), nullptr))
                fail("Could not create a staging folder for the mod.");
            created_parent_ = true;
        } else if (parent_attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            fail(unsafe_link_message);
        } else if (!(parent_attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            fail("The mod staging path is not a folder.");
        }
        verify_path_chain(parent, false);
        static std::atomic<std::uint64_t> sequence{};
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            path_ = parent / (L"install-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1)));
            if (CreateDirectoryW(path_.c_str(), nullptr)) return;
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                fail("Could not create a staging folder for the mod (Windows error " +
                     std::to_string(GetLastError()) + ").");
        }
        fail("Could not create a unique staging folder for the mod.");
    }
    ~Staging() {
        remove_tree_without_following_links(path_);
        const auto parent = path_.parent_path();
        const auto value = GetFileAttributesW(parent.c_str());
        if (created_parent_ && value != INVALID_FILE_ATTRIBUTES && (value & FILE_ATTRIBUTE_DIRECTORY) &&
            !(value & FILE_ATTRIBUTE_REPARSE_POINT)) RemoveDirectoryW(parent.c_str());
    }
    const fs::path& path() const { return path_; }
private:
    fs::path path_;
    bool created_parent_{};
};

// Moves the finished staging folder into place, replacing only when asked.
void commit(const fs::path& mods, const Staging& staging, const std::string& name, bool replace) {
    const auto target = mods / from_utf8(name);
    verify_path_chain(mods, false);
    verify_path_chain(staging.path(), false);
    validate_tree_without_links(staging.path());
    const auto value = GetFileAttributesW(target.c_str());
    const bool exists = value != INVALID_FILE_ATTRIBUTES;
    if (exists && (value & FILE_ATTRIBUTE_REPARSE_POINT)) fail(unsafe_link_message);
    const auto target_error = exists ? ERROR_SUCCESS : GetLastError();
    if (!exists && target_error != ERROR_FILE_NOT_FOUND && target_error != ERROR_PATH_NOT_FOUND)
        fail("Could not inspect the existing mod folder.");
    if (exists) {
        if (!(value & FILE_ATTRIBUTE_DIRECTORY)) fail("The existing mod path is not a folder.");
        if (!replace) throw AlreadyInstalled(name);
        validate_tree_without_links(target);
        recycle(target);
    }
    if (!MoveFileExW(staging.path().c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
        fail("Could not move the mod into the Mods folder (Windows error " + std::to_string(GetLastError()) + ").");
}

// miniz is built without stdio, so the archive is read through a handle.
struct Zip {
    mz_zip_archive archive{};
    HANDLE file{INVALID_HANDLE_VALUE};
    ~Zip() {
        mz_zip_reader_end(&archive);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

std::size_t read_at(void* opaque, mz_uint64 offset, void* buffer, std::size_t size) {
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD read{};
    if (size > MAXDWORD || !ReadFile(static_cast<HANDLE>(opaque), buffer, static_cast<DWORD>(size), &read, &position)) return 0;
    return read;
}

struct Sink {
    HANDLE file{INVALID_HANDLE_VALUE};
    std::uint64_t* written{};
    std::uint64_t total{};
    const Progress* progress{};
    const std::atomic<bool>* cancel{};
};

std::size_t write_chunk(void* opaque, mz_uint64, const void* data, std::size_t size) {
    auto& sink = *static_cast<Sink*>(opaque);
    if (sink.cancel->load()) return 0;
    if (size > MAXDWORD) return 0;
    DWORD written{};
    if (!WriteFile(sink.file, data, static_cast<DWORD>(size), &written, nullptr) || written != size) return 0;
    *sink.written += size;
    if (*sink.progress && sink.total) (*sink.progress)(static_cast<float>(*sink.written) / static_cast<float>(sink.total));
    return size;
}

std::string install_zip(const fs::path& mods, const fs::path& source, bool replace,
                        const Progress& progress, const std::atomic<bool>& cancel, const InstallOptions& options) {
    Zip zip;
    zip.file = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER size{};
    if (zip.file == INVALID_HANDLE_VALUE || !GetFileSizeEx(zip.file, &size))
        fail("Could not open " + utf8(source.filename().wstring()) + ".");
    zip.archive.m_pRead = read_at;
    zip.archive.m_pIO_opaque = zip.file;
    if (size.QuadPart <= 0 || !mz_zip_reader_init(&zip.archive, static_cast<mz_uint64>(size.QuadPart), 0))
        fail(utf8(source.filename().wstring()) + " is not a readable .zip file.");

    struct Entry { mz_uint index; std::string name; bool directory; std::uint64_t size; };
    std::vector<Entry> entries;
    std::vector<std::string> files;
    const auto count = mz_zip_reader_get_num_files(&zip.archive);
    for (mz_uint index = 0; index < count; ++index) {
        if (cancel) fail("Install cancelled.");
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip.archive, index, &stat)) fail("The .zip file is damaged.");
        const auto name = safe_entry(stat.m_filename);
        if (!name) fail("The .zip file contains an unsafe path (" + std::string(stat.m_filename) + "); it was not installed.");
        if (name->empty()) continue;
        const bool directory = mz_zip_reader_is_file_a_directory(&zip.archive, index);
        entries.push_back({index, *name, directory, stat.m_uncomp_size});
        if (!directory) files.push_back(*name);
    }
    const auto root = mod_root(files);
    if (!root) fail(utf8(source.filename().wstring()) + no_mod);
    const auto name = mod_name(*root, utf8(source.stem().wstring()), options);
    std::error_code error;
    if (!replace && fs::exists(mods / from_utf8(name), error)) throw AlreadyInstalled(name);

    std::uint64_t total = 0, written = 0;
    for (const auto& entry : entries)
        if (!entry.directory && relative_in_mod(*root, files, entry.name)) total += entry.size;
    check_free_space(mods, total);
    Staging staging(mods, name);
    for (const auto& entry : entries) {
        if (cancel) fail("Install cancelled.");
        const auto mapped = relative_in_mod(*root, files, entry.name);
        if (!mapped) continue;
        const auto& relative = *mapped;
        const auto safe = safe_relative_path(relative);
        if (!safe) fail("The .zip file contains an unsafe path (" + relative + "); it was not installed.");
        const auto target = staging.path() / from_utf8(*safe);
        if (!path_within(staging.path(), target)) fail("The .zip file contains a path outside its installation folder.");
        if (entry.directory) {
            ensure_destination_parent(staging.path(), target / L"entry");
            continue;
        }
        ensure_destination_parent(staging.path(), target);
        auto output = open_staging_file(staging.path(), target);
        Sink sink{output.value, &written, total, &progress, &cancel};
        const bool ok = mz_zip_reader_extract_to_callback(&zip.archive, entry.index, write_chunk, &sink, 0);
        if (cancel) fail("Install cancelled.");
        if (!ok) fail("Could not extract " + relative + " from the .zip file.");
    }
    finish(staging.path(), *root, utf8(source.filename().wstring()), options);
    // Cancellation is safe before replacing the installed folder, not midway through commit.
    if (progress) progress(1.f);
    if (cancel) fail("Install cancelled.");
    commit(mods, staging, name, replace);
    return name;
}

std::string install_folder(const fs::path& mods, const fs::path& source, bool replace,
                           const Progress& progress, const std::atomic<bool>& cancel, const InstallOptions& options) {
    // fs::relative() resolves filesystem links and can turn a link inside the
    // source into a parent traversal. Keep archive-style names lexical, reject
    // reparse points, and recheck staging parents before opening destination
    // handles. These path checks narrow races but cannot prevent another
    // same-user process changing a parent between validation and path-based
    // operations; this is not a full handle-relative TOCTOU defense.
    verify_path_chain(source, false);
    const auto source_root = fs::absolute(source).lexically_normal();
    std::vector<std::string> files;
    std::vector<fs::path> paths;
    std::vector<std::uint64_t> sizes;
    for (auto it = fs::recursive_directory_iterator(source_root); it != fs::recursive_directory_iterator(); ++it) {
        if (cancel) fail("Install cancelled.");
        const auto value = GetFileAttributesW(it->path().c_str());
        if (value == INVALID_FILE_ATTRIBUTES) fail("Could not inspect a file in the mod folder.");
        if (value & FILE_ATTRIBUTE_REPARSE_POINT) fail(unsafe_link_message);
        if (value & FILE_ATTRIBUTE_DIRECTORY) continue;
        const auto status = fs::symlink_status(it->path());
        if (!fs::is_regular_file(status)) continue;
        const auto lexical = it->path().lexically_relative(source_root);
        const auto relative = safe_relative_path(utf8(lexical.generic_wstring()));
        if (!relative) fail("The mod folder contains an unsafe path and was not installed.");
        paths.push_back(it->path());
        files.push_back(*relative);
        Handle file{CreateFileW(it->path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        BY_HANDLE_FILE_INFORMATION info{};
        if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            fail(unsafe_link_message);
        sizes.push_back((static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow);
    }
    const auto root = mod_root(files);
    if (!root) fail(utf8(source.filename().wstring()) + no_mod);
    const auto name = mod_name(*root, utf8(source.filename().wstring()), options);
    std::error_code error;
    if (!replace && fs::exists(mods / from_utf8(name), error)) throw AlreadyInstalled(name);

    std::uint64_t total = 0, written = 0;
    for (std::size_t i = 0; i < files.size(); ++i)
        if (relative_in_mod(*root, files, files[i])) total += sizes[i];
    Staging staging(mods, name);
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (cancel) fail("Install cancelled.");
        const auto relative = relative_in_mod(*root, files, files[i]);
        if (!relative) continue;
        const auto safe = safe_relative_path(*relative);
        if (!safe) fail("The mod folder contains an unsafe path and was not installed.");
        const auto target = staging.path() / from_utf8(*safe);
        copy_to_staging(paths[i], staging.path(), target);
        written += sizes[i];
        if (progress && total) progress(static_cast<float>(written) / static_cast<float>(total));
    }
    finish(staging.path(), *root, utf8(source.filename().wstring()), options);
    if (progress) progress(1.f);
    if (cancel) fail("Install cancelled.");
    commit(mods, staging, name, replace);
    return name;
}

} // namespace

fs::path mods_root(const fs::path& game_directory) {
    const auto data = launcher::mod_data_arguments(game_directory, {}).empty()
        ? game_directory : game_directory / L"ModData" / L"Default";
    return data / mods::mods_folder;
}

std::string install(const fs::path& mods, const fs::path& source, bool replace,
                    const Progress& progress, const std::atomic<bool>& cancel, const InstallOptions& options) {
    try {
        const auto mods_directory = fs::absolute(mods).lexically_normal();
        const auto source_path = fs::absolute(source).lexically_normal();
        verify_path_chain(mods_directory, true);
        std::error_code error;
        if (fs::is_directory(source_path, error)) {
            std::error_code same;
            if (fs::equivalent(source_path.parent_path(), mods_directory, same))
                fail(utf8(source_path.filename().wstring()) + " is already in the Mods folder.");
            return install_folder(mods_directory, source_path, replace, progress, cancel, options);
        }
        if (lower(utf8(source_path.extension().wstring())) != ".zip")
            fail("Choose a .zip file or a mod folder to install.");
        return install_zip(mods_directory, source_path, replace, progress, cancel, options);
    } catch (const AlreadyInstalled&) {
        throw;
    } catch (const fs::filesystem_error& failure) {
        fail(std::string("Install failed: ") + failure.what());
    }
}

void remove(const fs::path& mods, const std::string& name) {
    if (!mods::valid_mod_name(name)) fail("\"" + name + "\" is not a mod folder.");
    const auto target = mods / from_utf8(name);
    std::error_code error;
    if (!fs::is_directory(target, error)) fail("\"" + name + "\" is not installed.");
    recycle(target);
}

} // namespace dingosdk::launcher_mods
