#include "mod_merge_internal.h"
#include "Engine/Core/Json/json.h"
#include "Engine/Core/Platform/path_text.h"

#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace dingosdk::mods::detail {
namespace {
constexpr std::size_t maximumTocBytes = 64 * 1024 * 1024;
} // namespace

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& ch : result) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return result;
}

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open " + path_utf8(path));
    const auto length = input.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > maximumTocBytes)
        throw std::runtime_error("File exceeds the TOC size limit: " + path_utf8(path));
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!bytes.empty() &&
        !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read " + path_utf8(path));
    return bytes;
}

void write_file(const fs::path& path, std::span<const std::byte> bytes) {
    fs::create_directories(path.parent_path());
    const auto temporary = fs::path(path.wstring() + L".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(reinterpret_cast<const char*>(bytes.data()),
                                     static_cast<std::streamsize>(bytes.size())) || !output.flush()) // ~ofstream ignores a failed flush
            throw std::runtime_error("Cannot write " + path_utf8(temporary));
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Cannot publish " + path_utf8(path) +
                                 " (Windows error " + std::to_string(error) + ")");
    }
}

// A mod's archive is hard linked into the patch, so appending to it would write
// straight into the mod's own folder. Anything the merge adds to an archive
// therefore gives it a private copy first.
void unshare(const fs::path& path) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    BY_HANDLE_FILE_INFORMATION information{};
    const auto known = GetFileInformationByHandle(handle, &information) != FALSE;
    CloseHandle(handle);
    if (!known || information.nNumberOfLinks <= 1) return;
    const auto temporary = fs::path(path.wstring() + L".unshared");
    std::error_code error;
    fs::copy_file(path, temporary, fs::copy_options::overwrite_existing, error);
    if (error) throw std::runtime_error("Cannot give " + path_utf8(path) + " a private copy: " +
                                        error.message());
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const auto failure = GetLastError();
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Cannot replace the shared " + path_utf8(path) +
                                 " (Windows error " + std::to_string(failure) + ")");
    }
}

std::uint64_t append_file(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    unshare(to);
    std::ifstream input(from, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read " + path_utf8(from));
    // tellp() on a stream opened for append reads 0 until the first write, so
    // the block's start comes from the file itself.
    std::error_code error;
    const auto existing = fs::file_size(to, error);
    const auto start = error ? std::uint64_t{} : existing;
    std::ofstream output(to, std::ios::binary | std::ios::app);
    if (!output) throw std::runtime_error("Cannot append to " + path_utf8(to));
    // A megabyte of stack is a stack overflow; the copy buffer lives on the heap.
    std::vector<char> buffer(1 << 20);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (input.gcount() && !output.write(buffer.data(), input.gcount()))
            throw std::runtime_error("Cannot append to " + path_utf8(to));
    }
    if (!output) throw std::runtime_error("Cannot append to " + path_utf8(to));
    return start;
}

// Hard link when the volume allows it, so a 400 MB archive costs nothing.
void link_or_copy(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    std::error_code error;
    fs::remove(to, error);
    if (CreateHardLinkW(to.c_str(), from.c_str(), nullptr)) return;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, error);
    if (error) throw std::runtime_error("Cannot place " + path_utf8(to) + ": " + error.message());
}

RelativeFiles scan(const fs::path& directory) {
    RelativeFiles result;
    std::error_code error;
    const auto root = directory / L"Win32";
    if (!fs::is_directory(root, error) || error) return result;
    for (fs::recursive_directory_iterator it(root, error), end; it != end && !error; it.increment(error)) {
        if (!it->is_regular_file(error) || error) { error.clear(); continue; }
        // The game only names archives in ASCII: anything else is not one of its files.
        const auto relative = ascii_path(fs::relative(it->path(), directory, error));
        if (error || relative.empty()) { error.clear(); continue; }
        const auto extension = lower(ascii_path(it->path().extension()));
        if (extension == ".toc") result.tocs.push_back(relative);
        else if (extension == ".cas") result.archives.push_back(relative);
    }
    std::ranges::sort(result.tocs);
    std::ranges::sort(result.archives);
    return result;
}

fb::Sha1 sha1_of(std::span<const std::byte> bytes) {
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot open SHA-1");
    fb::Sha1 digest;
    const auto status = BCryptHash(algorithm, nullptr, 0,
        const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(bytes.data())),
        static_cast<ULONG>(bytes.size()),
        reinterpret_cast<PUCHAR>(digest.bytes.data()),
        static_cast<ULONG>(digest.bytes.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Cannot hash the merged asset");
    return digest;
}

PlacementRecord read_placements(const fs::path& file) {
    PlacementRecord record;
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("The launch's archive placements are missing; restart the game to merge mods");
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto root = Json::parse(text);
    for (const auto& [name, spots] : root.at("mods").items()) {
        auto& placement = record.mods[name];
        for (std::size_t i = 0; i < spots.size(); ++i) {
            const auto& spot = spots.at(i);
            placement.at[{spot.at("directory").string(), spot.at("number").get<std::uint16_t>()}] =
                {spot.at("archive").get<std::uint16_t>(), spot.at("offset").get<std::uint64_t>()};
        }
    }
    const auto& tocs = root.at("tocs");
    for (std::size_t i = 0; i < tocs.size(); ++i) record.tocs.push_back(tocs.at(i).string());
    if (root.contains("root")) {
        const auto& order = root.at("root");
        for (std::size_t i = 0; i < order.size(); ++i) record.root.push_back(order.at(i).string());
    }
    return record;
}

void write_placements(const fs::path& file, const PlacementRecord& record) {
    auto root = Json::object();
    root["schema"] = 1u;
    auto mods = Json::object();
    for (const auto& [name, placement] : record.mods) {
        auto spots = Json::array();
        for (const auto& [where, spot] : placement.at) {
            auto row = Json::object();
            row["directory"] = where.first;
            row["number"] = static_cast<unsigned>(where.second);
            row["archive"] = static_cast<unsigned>(spot.archive);
            row["offset"] = spot.offset;
            spots.push_back(std::move(row));
        }
        mods[name] = std::move(spots);
    }
    root["mods"] = std::move(mods);
    auto tocs = Json::array();
    for (const auto& toc : record.tocs) tocs.push_back(toc);
    root["tocs"] = std::move(tocs);
    auto order = Json::array();
    for (const auto& name : record.root) order.push_back(name);
    root["root"] = std::move(order);
    const auto text = root.dump(2);
    write_file(file, std::as_bytes(std::span(text)));
}

} // namespace dingosdk::mods::detail
