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

// What the game loads from a mod (a plugin's reskate-plugin.json sits at its
// mod's top), and the files that only describe one.
constexpr std::array<std::string_view, 3> content_markers{"layout.toc", "reskate-levels.json", "reskate-plugin.json"};
constexpr std::array<std::string_view, 2> info_markers{"manifest.json", "reskate-mod.json"};
// Thunderstore requires these at the top of a package (lower case here); they
// follow the mod in when it is packed one folder deeper.
constexpr std::array<std::string_view, 4> package_files{"manifest.json", "icon.png", "readme.md", "changelog.md"};
constexpr char no_mod[] =
    " does not contain a ReSkate mod (no layout.toc, reskate-levels.json, reskate-plugin.json or manifest.json).";
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

// The shallowest folder holding game content (layout.toc, reskate-levels.json,
// reskate-plugin.json or parks/<map>.park.json), else the shallowest holding
// only a description.
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

// The mod's manifest.json as an object, if it has a readable one. An unreadable
// manifest is the mod's own business; the game reports it.
std::optional<Json> read_manifest(const fs::path& manifest) {
    std::error_code error;
    if (!fs::is_regular_file(manifest, error)) return std::nullopt;
    try {
        std::string text;
        {
            std::ifstream input(manifest, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(input), {});
        }
        if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
        auto json = Json::parse(text, JsonLimits{1024 * 1024, 16, 65536});
        if (json.is_object()) return json;
    } catch (...) {
    }
    return std::nullopt;
}

// Last steps in staging: a source that gives the game nothing to load is
// refused when asked, unless its manifest.json lists dependencies (a mod pack:
// what it brings is those), and the author goes into manifest.json.
void finish(const fs::path& staging, const Root& root, const std::string& source_name, const InstallOptions& options) {
    const auto manifest = staging / mods::manifest_file;
    auto json = read_manifest(manifest);
    const bool pack = json && json->contains("dependencies") && json->at("dependencies").is_array() &&
        !json->at("dependencies").empty();
    if (options.require_content && !root.content && !pack)
        fail(source_name + " has nothing for ReSkate to load: no layout.toc, reskate-levels.json, reskate-plugin.json "
             "or parks folder, and no dependencies. Only mods built with ReSkate Studio, park mods, plugins and mod "
             "packs can be installed.");
    if (options.author.empty() || !json) return;
    if (json->contains("author") && json->at("author").is_string() && !json->at("author").string().empty()) return;
    json->items()["author"] = Json(options.author);
    std::ofstream(manifest, std::ios::binary | std::ios::trunc) << json->dump(4) << '\n';
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

// Moves the finished staging folder into place, replacing only when asked. The copy it
// replaces (an update, mostly) is deleted for good rather than recycled: it is moved aside
// into the staging folder first, so a new copy that cannot go in leaves the old one where it was.
void commit(const fs::path& mods, const Staging& staging, const std::string& name, bool replace) {
    const auto target = mods / from_utf8(name);
    std::error_code error;
    fs::path aside;
    if (fs::exists(target, error)) {
        if (!replace) throw AlreadyInstalled(name);
        aside = staging.path().parent_path() / (from_utf8(name).wstring() + L".replaced");
        fs::remove_all(aside, error);
        if (!MoveFileExW(target.c_str(), aside.c_str(), MOVEFILE_WRITE_THROUGH))
            fail("Could not replace " + name + ": close anything using its files (the game, an editor) and try again.");
    }
    if (!MoveFileExW(staging.path().c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError();
        if (!aside.empty()) MoveFileExW(aside.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
        fail("Could not move the mod into the Mods folder (Windows error " + std::to_string(code) + ").");
    }
    // (Whatever of it cannot be deleted now goes with the staging folder.)
    if (!aside.empty()) fs::remove_all(aside, error);
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
    // Cancellation is safe before replacing the installed folder, not midway through commit.
    if (progress) progress(1.f);
    if (cancel) fail("Install cancelled.");
    commit(mods, staging, name, replace);
    return name;
}

// A symbolic link, junction or mount point: an entry that names a path somewhere else. (Cloud
// placeholders such as OneDrive's are reparse points too, but name nothing else.)
bool links_elsewhere(const fs::path& path) {
    WIN32_FIND_DATAW found{};
    const auto search = FindFirstFileW(path.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return false;
    FindClose(search);
    return (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(found.dwReserved0);
}

// A folder's files are named by where they sit in it, never by where a link leads, and checked
// as a zip's entries are. fs::relative followed links: a file link inside the folder to one
// outside it came out as ../../<file>, which was copied over that file (issue #265, first fixed
// in PR #267 by Jean-LucTch). So a link inside the folder refuses the install. Links on the way
// to the folder or to the Mods folder (a library moved to another drive) are the player's own
// and stay fine.
std::string install_folder(const fs::path& mods, const fs::path& source, bool replace,
                           const Progress& progress, const std::atomic<bool>& cancel, const InstallOptions& options) {
    std::vector<std::string> files;
    std::vector<fs::path> paths;
    for (auto it = fs::recursive_directory_iterator(source); it != fs::recursive_directory_iterator(); ++it) {
        if (cancel) fail("Install cancelled.");
        const auto inside = it->path().lexically_relative(source);
        if (links_elsewhere(it->path()))
            fail(utf8(source.filename().wstring()) + " holds a link (" + utf8(inside.generic_wstring()) +
                 "), which a mod folder may not: copy the real files in and try again.");
        if (!fs::is_regular_file(it->symlink_status())) continue;
        auto relative = safe_entry(utf8(inside.generic_wstring()));
        if (!relative) fail("The folder contains an unsafe path (" + utf8(inside.generic_wstring()) + "); it was not installed.");
        if (relative->empty()) continue;
        paths.push_back(it->path());
        files.push_back(std::move(*relative));
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

namespace {
// The mod folders `folder`'s manifest.json lists as dependencies, folded to lower case.
std::vector<std::string> listed_dependencies(const fs::path& folder) {
    std::vector<std::string> result;
    try {
        std::string text;
        {
            std::ifstream input(folder / mods::manifest_file, std::ios::binary);
            if (!input) return result;
            text.assign(std::istreambuf_iterator<char>(input), {});
        }
        if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
        const auto json = Json::parse(text, JsonLimits{1024 * 1024, 16, 65536});
        if (!json.is_object() || !json.contains("dependencies") || !json.at("dependencies").is_array()) return result;
        for (const auto& item : json.at("dependencies")) {
            if (!item.is_string()) continue;
            const auto& name = item.string();
            // Namespace-Name-1.2.3: the folder is everything before the version.
            const auto dash = name.rfind('-');
            if (dash == std::string::npos || name.find('-') == dash) continue;
            // (A long name installs cut to the longest folder name, as thunderstore::folder_for has it.)
            result.push_back(lower(std::string_view(name).substr(0, std::min(dash, mods::maximum_mod_name))));
        }
    } catch (...) {
        result.clear();
    }
    return result;
}

Json read_brought_in(const fs::path& mods) {
    try {
        std::ifstream input(mods / brought_in_file, std::ios::binary);
        if (!input) return Json::object();
        const std::string text{std::istreambuf_iterator<char>(input), {}};
        auto json = Json::parse(text, JsonLimits{4 * 1024 * 1024, 8, 65536});
        return json.is_object() ? json : Json::object();
    } catch (...) {
        return Json::object();
    }
}

void write_brought_in(const fs::path& mods, const Json& json) {
    const auto target = mods / brought_in_file;
    auto temporary = target;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not write " + utf8(target.filename().wstring()) + ".");
        output << json.dump(2) << '\n';
    }
    std::error_code error;
    fs::rename(temporary, target, error);
    if (error) throw std::runtime_error("Could not write " + utf8(target.filename().wstring()) + ".");
}
}

std::vector<std::string> brought_in(const fs::path& mods, const std::string& pack) {
    std::vector<std::string> result;
    const auto json = read_brought_in(mods);
    for (const auto& [name, list] : json.items())
        if (lower(name) == lower(pack) && list.is_array())
            for (const auto& item : list)
                if (item.is_string() && mods::valid_mod_name(item.string())) result.push_back(item.string());
    return result;
}

void record_brought_in(const fs::path& mods, const std::string& pack, const std::vector<std::string>& dependencies) {
    if (dependencies.empty()) return;
    auto json = read_brought_in(mods);
    auto names = brought_in(mods, pack);
    for (const auto& name : dependencies)
        if (std::none_of(names.begin(), names.end(), [&](const std::string& have) { return lower(have) == lower(name); }))
            names.push_back(name);
    auto list = Json::array();
    for (const auto& name : names) list.push_back(Json(name));
    for (auto it = json.items().begin(); it != json.items().end();)
        it = lower(it->first) == lower(pack) ? json.items().erase(it) : std::next(it);
    json.items()[pack] = std::move(list);
    write_brought_in(mods, json);
}

void forget_brought_in(const fs::path& mods, const std::vector<std::string>& removed) {
    auto json = read_brought_in(mods);
    bool changed = false;
    for (auto it = json.items().begin(); it != json.items().end();) {
        const bool gone = std::any_of(removed.begin(), removed.end(), [&](const std::string& name) { return lower(name) == lower(it->first); });
        if (gone) { it = json.items().erase(it); changed = true; }
        else ++it;
    }
    if (changed) write_brought_in(mods, json);
}

std::vector<std::string> orphaned_dependencies(const fs::path& mods, const std::vector<std::string>& removing,
                                               const std::vector<std::string>& installed) {
    std::vector<std::string> folded;
    for (const auto& name : installed) folded.push_back(lower(name));
    const auto is_installed = [&](const std::string& name) { return std::find(folded.begin(), folded.end(), name) != folded.end(); };
    std::vector<std::string> gone;
    for (const auto& name : removing) gone.push_back(lower(name));
    const auto listed = [&](const std::string& name) {
        const auto at = std::find(folded.begin(), folded.end(), name);
        return at == folded.end() ? std::vector<std::string>{} : listed_dependencies(mods / from_utf8(installed[at - folded.begin()]));
    };
    // What the removed mods brought in when they were installed (their dependencies' dependencies too):
    // only these, so a mod that was installed before them is never offered.
    std::vector<std::string> candidates;
    for (const auto& pack : removing)
        for (const auto& name : brought_in(mods, pack)) {
            const auto dependency = lower(name);
            if (is_installed(dependency) && std::find(gone.begin(), gone.end(), dependency) == gone.end() &&
                std::find(candidates.begin(), candidates.end(), dependency) == candidates.end())
                candidates.push_back(dependency);
        }
    // What a mod that stays still lists stays too, with what that one lists in turn.
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < folded.size(); ++i) {
            const auto& name = folded[i];
            if (std::find(gone.begin(), gone.end(), name) != gone.end() ||
                std::find(candidates.begin(), candidates.end(), name) != candidates.end())
                continue;
            for (const auto& dependency : listed(name))
                if (const auto at = std::find(candidates.begin(), candidates.end(), dependency); at != candidates.end()) {
                    candidates.erase(at);
                    changed = true;
                }
        }
    }
    std::vector<std::string> result;
    for (std::size_t i = 0; i < folded.size(); ++i)
        if (std::find(candidates.begin(), candidates.end(), folded[i]) != candidates.end()) result.push_back(installed[i]);
    return result;
}

std::string game_data_signature(const std::vector<DataMod>& mods, const std::map<std::string, std::string, std::less<>>& variants) {
    std::string text;
    for (const auto& mod : mods) text += mod.name + '=' + mods::mod_fingerprint(mod.directory) + ';';
    for (const auto& [mod, variant] : variants) text += "variant " + mod + '=' + variant + ';';
    return text;
}

std::filesystem::path game_shader_cache() {
    std::array<wchar_t, 32768> local{};
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), static_cast<DWORD>(local.size()));
    if (!size || size >= local.size()) return {};
    return fs::path(local.data()) / L"ReSkate" / L"Game" / L"Skate" / L"cache";
}

std::vector<std::string> empty_folder(const std::filesystem::path& folder) {
    std::vector<std::string> failed;
    std::error_code error;
    if (folder.empty() || !fs::is_directory(folder, error)) return failed;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        std::error_code removing;
        fs::remove_all(it->path(), removing);
        if (removing) failed.push_back(utf8(it->path().filename().wstring()) + ": " + removing.message());
    }
    if (error) failed.push_back("the folder could not be read: " + error.message());
    return failed;
}

void remove(const fs::path& mods, const std::string& name) {
    if (!mods::valid_mod_name(name)) fail("\"" + name + "\" is not a mod folder.");
    const auto target = mods / from_utf8(name);
    std::error_code error;
    if (!fs::is_directory(target, error)) fail("\"" + name + "\" is not installed.");
    recycle(target);
}

namespace {
constexpr wchar_t removals_file[] = L"remove.txt";
fs::path pending_root(const fs::path& mods) { return mods / from_utf8(pending_folder); }
std::vector<std::string> read_removals(const fs::path& mods) {
    std::vector<std::string> names;
    std::ifstream in(pending_root(mods) / removals_file);
    for (std::string line; std::getline(in, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (mods::valid_mod_name(line) && std::find(names.begin(), names.end(), line) == names.end()) names.push_back(line);
    }
    return names;
}
// Replaces remove.txt (or removes it when there is nothing in it).
void write_removals(const fs::path& mods, const std::vector<std::string>& names) {
    const auto file = pending_root(mods) / removals_file;
    std::error_code error;
    if (names.empty()) {
        fs::remove(file, error);
        return;
    }
    fs::create_directories(file.parent_path());
    const auto temporary = fs::path(file).concat(L".tmp");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        for (const auto& name : names) out << name << '\n';
        if (!out) fail("Could not note the mod to uninstall (is the disk full?).");
    }
    if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        fail("Could not note the mod to uninstall.");
}
} // namespace

std::string install_later(const fs::path& mods, const fs::path& source, const Progress& progress,
                          const std::atomic<bool>& cancel, const InstallOptions& options) {
    const auto name = install(pending_root(mods), source, true, progress, cancel, options);
    auto removals = read_removals(mods);
    if (std::erase(removals, name)) write_removals(mods, removals);
    return name;
}

void remove_later(const fs::path& mods, const std::string& name) {
    if (!mods::valid_mod_name(name)) fail("\"" + name + "\" is not a mod folder.");
    std::error_code error;
    fs::remove_all(pending_root(mods) / from_utf8(name), error);
    auto removals = read_removals(mods);
    if (std::find(removals.begin(), removals.end(), name) == removals.end()) removals.push_back(name);
    write_removals(mods, removals);
}

Pending pending(const fs::path& mods) noexcept {
    Pending result;
    try {
        std::error_code error;
        for (fs::directory_iterator it(pending_root(mods), error), end; !error && it != end; it.increment(error)) {
            const auto name = utf8(it->path().filename().wstring());
            // (install()'s own staging folder is a dot folder too.)
            if (it->is_directory(error) && mods::valid_mod_name(name) && !name.ends_with(".replaced")) result.updates.push_back(name);
        }
        result.removals = read_removals(mods);
    } catch (...) {}
    return result;
}

std::vector<std::string> apply_pending(const fs::path& mods) noexcept {
    std::vector<std::string> lines;
    try {
        const auto waiting = pending(mods);
        if (waiting.updates.empty() && waiting.removals.empty()) return lines;
        const auto root = pending_root(mods);
        for (const auto& name : waiting.updates) {
            const auto target = mods / from_utf8(name), copy = root / from_utf8(name);
            const auto aside = root / (from_utf8(name).wstring() + L".replaced");
            std::error_code error;
            fs::remove_all(aside, error);
            if (fs::exists(target, error) && !MoveFileExW(target.c_str(), aside.c_str(), MOVEFILE_WRITE_THROUGH)) {
                lines.push_back("The update of " + name + " waits: its folder is in use (Windows error " + std::to_string(GetLastError()) + ").");
                continue;
            }
            if (!MoveFileExW(copy.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                const auto code = GetLastError();
                MoveFileExW(aside.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
                lines.push_back("The update of " + name + " waits: it could not be moved in (Windows error " + std::to_string(code) + ").");
                continue;
            }
            fs::remove_all(aside, error);
            lines.push_back("Updated " + name + " (installed in game, waiting for this start).");
        }
        std::vector<std::string> kept;
        for (const auto& name : waiting.removals) {
            std::error_code error;
            if (!fs::is_directory(mods / from_utf8(name), error)) continue;
            try {
                recycle(mods / from_utf8(name));
                lines.push_back("Uninstalled " + name + " (asked for in game).");
            } catch (const std::exception& failure) {
                kept.push_back(name);
                lines.push_back("Uninstalling " + name + " waits: " + failure.what());
            }
        }
        try { write_removals(mods, kept); } catch (const std::exception& failure) { lines.push_back(failure.what()); }
        std::error_code error;
        if (fs::is_empty(root, error)) fs::remove(root, error);
    } catch (const std::exception& failure) {
        lines.push_back(std::string("Mod changes waiting from the game could not be made: ") + failure.what());
    } catch (...) {}
    return lines;
}

} // namespace dingosdk::launcher_mods
