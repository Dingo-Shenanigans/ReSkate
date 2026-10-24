#include "builtin_packs.h"
#include "builtin_pack_format.h"
#include "mod_list.h"
#include "Engine/Core/Platform/path_text.h"
#include <Windows.h>
#include <miniz.h>
#include <zstd.h>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <format>
#include <fstream>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace dingosdk::mods {
namespace {
namespace fs = std::filesystem;
// Each pack: its RCDATA resource in ReSkate.dll (cmake/Runtime.cmake) and its folder name.
struct Embedded {
    const wchar_t* resource;
    const char* name;
};
constexpr Embedded embedded[]{{L"BUILTIN_PACK_THROWDOWN_MENU", "throwdown_menu"}};
// Retired mods (builtin_packs.h), in small letters.
constexpr std::string_view retired[]{"fourth_studio-board_break"};
std::string lower_text(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}
constexpr std::uint64_t max_pack_bytes = builtin_pack::max_bytes;
constexpr std::string_view version_file = ".builtin-version";

struct Source {
    HMODULE module{};
    bool owned{};
    Source() = default;
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
    ~Source() {
        if (owned && module) FreeLibrary(module);
    }
};
bool carries_packs(HMODULE module) {
    return module && std::any_of(std::begin(embedded), std::end(embedded), [&](const Embedded& pack) {
        return FindResourceW(module, pack.resource, MAKEINTRESOURCEW(10) /* RT_RCDATA */) != nullptr;
    });
}
// ReSkate.dll: this module when it is the DLL (the game), else ReSkate.dll beside the program
// (the launcher, which sits beside Skate.exe and the DLL), as data only.
void open_source(Source& source) {
    HMODULE self{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&carries_packs), &self) && carries_packs(self)) {
        source.module = self;
        return;
    }
    if (const auto loaded = GetModuleHandleW(L"ReSkate.dll"); carries_packs(loaded)) {
        source.module = loaded;
        return;
    }
    std::wstring program(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, program.data(), static_cast<DWORD>(program.size()));
    if (!length || length >= program.size()) return;
    program.resize(length);
    const auto dll = fs::path(program).parent_path() / L"ReSkate.dll";
    source.module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    source.owned = source.module != nullptr;
}
std::span<const unsigned char> resource_bytes(HMODULE module, const wchar_t* name) {
    const auto resource = FindResourceW(module, name, MAKEINTRESOURCEW(10));
    if (!resource) return {};
    const auto loaded = LoadResource(module, resource);
    const auto size = SizeofResource(module, resource);
    const auto* data = loaded ? static_cast<const unsigned char*>(LockResource(loaded)) : nullptr;
    return data && size ? std::span<const unsigned char>(data, size) : std::span<const unsigned char>{};
}
// Which build of a pack this is: enough to tell when the one embedded changed.
std::string version_of(std::span<const unsigned char> bytes) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto byte : bytes) hash = (hash ^ byte) * 0x100000001b3ULL;
    return std::format("{:016x}-{}", hash, bytes.size());
}
std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
bool safe_entry(std::string_view name) { return builtin_pack::safe_name(name); }
// Writes `size` bytes to a new file at `path` (its folders made first).
bool write_file(const fs::path& path, const unsigned char* data, std::uint64_t size) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    struct File {
        HANDLE value;
        ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return false;
    while (size) {
        const auto chunk = static_cast<DWORD>(std::min<std::uint64_t>(size, 64ull * 1024 * 1024));
        DWORD done{};
        if (!WriteFile(file.value, data, chunk, &done, nullptr) || done != chunk) return false;
        data += chunk;
        size -= chunk;
    }
    return true;
}
// A pack in ReSkate's own format (builtin_pack_format.h): one zstd frame, unpacked whole.
bool unpack_packed(std::span<const unsigned char> bytes, const fs::path& target, std::string& issue) {
    const auto size = ZSTD_getFrameContentSize(bytes.data(), bytes.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > max_pack_bytes + 16ull * 1024 * 1024) {
        issue = "the embedded pack is not one ReSkate made";
        return false;
    }
    std::vector<unsigned char> plain(static_cast<std::size_t>(size));
    const auto unpacked = ZSTD_decompress(plain.data(), plain.size(), bytes.data(), bytes.size());
    if (ZSTD_isError(unpacked) || unpacked != plain.size()) {
        issue = std::string("the embedded pack could not be decompressed: ") + (ZSTD_isError(unpacked) ? ZSTD_getErrorName(unpacked) : "short");
        return false;
    }
    std::size_t at = 0;
    const auto take = [&](void* out, std::size_t count) {
        if (count > plain.size() - at) return false;
        std::memcpy(out, plain.data() + at, count);
        at += count;
        return true;
    };
    std::array<char, builtin_pack::magic.size()> magic{};
    std::uint32_t count{};
    if (!take(magic.data(), magic.size()) || magic != builtin_pack::magic || !take(&count, sizeof(count)) || count > 100000) {
        issue = "the embedded pack is not one ReSkate made";
        return false;
    }
    std::vector<std::pair<std::string, std::uint64_t>> files(count);
    std::uint64_t total{};
    for (auto& [name, length] : files) {
        std::uint16_t name_length{};
        if (!take(&name_length, sizeof(name_length))) { issue = "the embedded pack's list is cut short"; return false; }
        name.resize(name_length);
        if (!take(name.data(), name.size()) || !take(&length, sizeof(length))) { issue = "the embedded pack's list is cut short"; return false; }
        total += length;
        if (!safe_entry(name) || total > max_pack_bytes) {
            issue = "the embedded pack has an entry it may not: " + name;
            return false;
        }
    }
    for (const auto& [name, length] : files) {
        if (length > plain.size() - at) { issue = "the embedded pack is cut short at " + name; return false; }
        if (!write_file(target / fs::path(name), plain.data() + at, length)) { issue = "could not unpack " + name; return false; }
        at += static_cast<std::size_t>(length);
    }
    if (at != plain.size()) {
        issue = "the embedded pack has bytes past its last file";
        return false;
    }
    return true;
}
bool unpack(std::span<const unsigned char> bytes, const fs::path& target, std::string& issue) {
    // ReSkate's own format (a zstd frame); else a plain zip, as a build without the packer embeds.
    if (bytes.size() >= 4 && bytes[0] == 0x28 && bytes[1] == 0xb5 && bytes[2] == 0x2f && bytes[3] == 0xfd)
        return unpack_packed(bytes, target, issue);
    struct Zip {
        mz_zip_archive value{};
        ~Zip() { mz_zip_reader_end(&value); }
    } zip;
    if (!mz_zip_reader_init_mem(&zip.value, bytes.data(), bytes.size(), 0)) {
        issue = "the embedded pack is not a zip";
        return false;
    }
    const auto count = mz_zip_reader_get_num_files(&zip.value);
    std::uint64_t total{};
    for (mz_uint index = 0; index < count; ++index) {
        mz_zip_archive_file_stat info{};
        if (!mz_zip_reader_file_stat(&zip.value, index, &info) || info.m_is_encrypted) {
            issue = "the embedded pack has an unreadable entry";
            return false;
        }
        if (info.m_is_directory) continue;
        total += info.m_uncomp_size;
        if (!safe_entry(info.m_filename) || total > max_pack_bytes) {
            issue = std::string("the embedded pack has an entry it may not: ") + info.m_filename;
            return false;
        }
        const auto path = target / fs::path(info.m_filename);
        fs::create_directories(path.parent_path());
        struct File {
            HANDLE value;
            ~File() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        } file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        struct Sink {
            HANDLE file;
            std::uint64_t written{}, limit;
        } sink{file.value, 0, info.m_uncomp_size};
        const auto write = [](void* opaque, mz_uint64 offset, const void* buffer, size_t size) -> size_t {
            auto& target = *static_cast<Sink*>(opaque);
            if (offset != target.written || size > target.limit - target.written) return 0;
            DWORD done{};
            if (!WriteFile(target.file, buffer, static_cast<DWORD>(size), &done, nullptr) || done != size) return 0;
            target.written += size;
            return size;
        };
        if (file.value == INVALID_HANDLE_VALUE || !mz_zip_reader_extract_to_callback(&zip.value, index, write, &sink, 0) ||
            sink.written != info.m_uncomp_size) {
            issue = std::string("could not unpack ") + info.m_filename;
            return false;
        }
    }
    return true;
}
} // namespace

bool unpack_builtin_pack(std::span<const unsigned char> bytes, const fs::path& target, std::string& issue) noexcept {
    try {
        return unpack(bytes, target, issue);
    } catch (const std::exception& failure) {
        issue = failure.what();
        return false;
    }
}

bool retired_mod(std::string_view folder) noexcept {
    try {
        return std::find(std::begin(retired), std::end(retired), lower_text(folder)) != std::end(retired);
    } catch (...) {
        return false;
    }
}

bool kept_by_reskate(const fs::path&, std::string_view folder) noexcept {
    return !folder.empty() && retired_mod(folder);
}

std::vector<BuiltinPack> unpack_builtin_packs(const fs::path& mods_root, std::vector<std::string>& notes) noexcept {
    std::vector<BuiltinPack> packs;
    try {
        Source source;
        open_source(source);
        for (const auto& pack : embedded) {
            if (!source.module) break;
            const auto bytes = resource_bytes(source.module, pack.resource);
            if (bytes.empty()) continue;
            const auto directory = mods_root / std::string(builtin_folder) / pack.name;
            const auto version = version_of(bytes);
            std::error_code error;
            if (read_text(directory / version_file) != version) {
                // Unpacked beside it, then swapped in: a pack is never half there.
                const auto staging = fs::path(directory.wstring() + std::format(L".unpacking-{}", GetCurrentProcessId()));
                fs::remove_all(staging, error);
                std::string issue;
                if (!unpack(bytes, staging, issue)) {
                    fs::remove_all(staging, error);
                    notes.push_back(std::format("Built-in content {} could not be unpacked: {}", pack.name, issue));
                    continue;
                }
                std::ofstream(staging / version_file, std::ios::binary) << version;
                fs::remove_all(directory, error);
                fs::rename(staging, directory, error);
                if (error) {
                    fs::remove_all(staging, error);
                    notes.push_back(std::format("Built-in content {} could not be put in place ({})", pack.name, error.message()));
                    continue;
                }
                notes.push_back(std::format("Built-in content {} unpacked ({} bytes)", pack.name, bytes.size()));
            }
            packs.push_back({std::string("reskate-builtin-") + pack.name, directory});
        }
        // What an older ReSkate carried and this one does not (it became an official mod, or was
        // dropped) goes: the Board Break deck list reads every folder in here.
        std::error_code error;
        const auto folder = mods_root / std::string(builtin_folder);
        std::vector<fs::path> stale;
        for (fs::directory_iterator at(folder, error), end; !error && at != end; at.increment(error))
            if (std::none_of(packs.begin(), packs.end(), [&](const BuiltinPack& pack) { return pack.directory == at->path(); }))
                stale.push_back(at->path());
        for (const auto& path : stale) {
            fs::remove_all(path, error);
            if (!error) notes.push_back("Built-in content " + path_utf8(path.filename()) + " is no longer carried; removed");
        }
        if (packs.empty()) fs::remove(folder, error);
    } catch (const std::exception& failure) {
        notes.push_back(std::string("Built-in content could not be prepared: ") + failure.what());
    }
    return packs;
}
} // namespace dingosdk::mods
