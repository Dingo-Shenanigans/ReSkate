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
#include <cstdio>
#include <fstream>
#include <optional>
#include <string_view>
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
    std::error_code error;
    if (options.author.empty() || !fs::is_regular_file(manifest, error)) return;
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
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos) return std::nullopt;
    std::string_view rest(name);
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const auto part = rest.substr(0, slash);
        if (part == "..") return std::nullopt;
        if (slash == std::string_view::npos) break;
        rest.remove_prefix(slash + 1);
    }
    if (name.starts_with("__MACOSX/")) return std::string();
    return name;
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
    Staging(const fs::path& mods, const std::string& name) : path_(mods / staging_folder / from_utf8(name)) {
        std::error_code error;
        fs::remove_all(path_, error);
        fs::create_directories(path_);
    }
    ~Staging() {
        std::error_code error;
        fs::remove_all(path_.parent_path(), error);
    }
    const fs::path& path() const { return path_; }
private:
    fs::path path_;
};

// Moves the finished staging folder into place, replacing only when asked.
void commit(const fs::path& mods, const Staging& staging, const std::string& name, bool replace) {
    const auto target = mods / from_utf8(name);
    std::error_code error;
    if (fs::exists(target, error)) {
        if (!replace) throw AlreadyInstalled(name);
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
    FILE* file{};
    std::uint64_t* written{};
    std::uint64_t total{};
    const Progress* progress{};
    const std::atomic<bool>* cancel{};
};

std::size_t write_chunk(void* opaque, mz_uint64, const void* data, std::size_t size) {
    auto& sink = *static_cast<Sink*>(opaque);
    if (sink.cancel->load()) return 0;
    if (std::fwrite(data, 1, size, sink.file) != size) return 0;
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
        const auto target = staging.path() / from_utf8(relative);
        if (entry.directory) { fs::create_directories(target); continue; }
        fs::create_directories(target.parent_path());
        FILE* output{};
        if (_wfopen_s(&output, target.c_str(), L"wb") || !output) fail("Could not write " + relative + ".");
        Sink sink{output, &written, total, &progress, &cancel};
        const bool ok = mz_zip_reader_extract_to_callback(&zip.archive, entry.index, write_chunk, &sink, 0);
        std::fclose(output);
        if (cancel) fail("Install cancelled.");
        if (!ok) fail("Could not extract " + relative + " from the .zip file.");
    }
    finish(staging.path(), *root, utf8(source.filename().wstring()), options);
    commit(mods, staging, name, replace);
    return name;
}

std::string install_folder(const fs::path& mods, const fs::path& source, bool replace,
                           const Progress& progress, const std::atomic<bool>& cancel, const InstallOptions& options) {
    std::vector<std::string> files;
    std::vector<fs::path> paths;
    for (auto it = fs::recursive_directory_iterator(source); it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        paths.push_back(it->path());
        auto relative = utf8(fs::relative(it->path(), source).generic_wstring());
        files.push_back(std::move(relative));
    }
    const auto root = mod_root(files);
    if (!root) fail(utf8(source.filename().wstring()) + no_mod);
    const auto name = mod_name(*root, utf8(source.filename().wstring()), options);
    std::error_code error;
    if (!replace && fs::exists(mods / from_utf8(name), error)) throw AlreadyInstalled(name);

    std::uint64_t total = 0, written = 0;
    for (std::size_t i = 0; i < files.size(); ++i)
        if (relative_in_mod(*root, files, files[i])) total += fs::file_size(paths[i]);
    Staging staging(mods, name);
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (cancel) fail("Install cancelled.");
        const auto relative = relative_in_mod(*root, files, files[i]);
        if (!relative) continue;
        const auto target = staging.path() / from_utf8(*relative);
        fs::create_directories(target.parent_path());
        fs::copy_file(paths[i], target, fs::copy_options::overwrite_existing);
        written += fs::file_size(paths[i]);
        if (progress && total) progress(static_cast<float>(written) / static_cast<float>(total));
    }
    finish(staging.path(), *root, utf8(source.filename().wstring()), options);
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
        fs::create_directories(mods);
        std::error_code error;
        if (fs::is_directory(source, error)) {
            std::error_code same;
            if (fs::equivalent(source.parent_path(), mods, same))
                fail(utf8(source.filename().wstring()) + " is already in the Mods folder.");
            return install_folder(mods, source, replace, progress, cancel, options);
        }
        if (lower(utf8(source.extension().wstring())) != ".zip")
            fail("Choose a .zip file or a mod folder to install.");
        return install_zip(mods, source, replace, progress, cancel, options);
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
