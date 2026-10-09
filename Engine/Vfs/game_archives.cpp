#include "game_archives.h"
#include "Engine/Resource/cas_codec.h"
#include "Engine/Core/Platform/path_text.h"
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace dingosdk::vfs {
namespace fs = std::filesystem;
namespace fb = frostbite;
namespace {
std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}
}

std::string archive_file(std::uint16_t archive) {
    char name[32]{};
    std::snprintf(name, sizeof(name), "cas_%02u.cas", static_cast<unsigned>(archive));
    return name;
}

Layout read_layout(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Layout layout{{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()}, {}};
    if (layout.buffer.size() <= native_db::envelope_size ||
        std::memcmp(layout.buffer.data(), native_db::magic, sizeof(native_db::magic)))
        throw std::runtime_error("Unrecognized native envelope in " + path_utf8(path));
    layout.root = native_db::read(std::span<const unsigned char>(layout.buffer).subspan(native_db::envelope_size),
                                  "layout.toc", nullptr, {.unique_fields = false});
    return layout;
}

std::vector<std::uint32_t> GameArchives::chunks_in(const std::string& directory) const {
    std::vector<std::uint32_t> chunks;
    for (const auto& [chunk, name] : chunkDirectory_)
        if (name == directory) chunks.push_back(chunk);
    return chunks;
}

GameArchives::GameArchives(fs::path root, const native_db::Node& layout) : root_(std::move(root)) {
    std::map<std::set<std::uint16_t>, std::vector<std::string>> byArchives;
    std::error_code error;
    const auto win32 = root_ / "Win32";
    for (fs::recursive_directory_iterator it(win32, error), end; it != end && !error; it.increment(error)) {
        if (!it->is_regular_file(error) || error) { error.clear(); continue; }
        if (lower(it->path().extension().string()) != ".cas") continue;
        const auto index = archive_index(it->path().stem().string());
        if (!index) continue;
        // Relative to Win32, because read() adds that itself.
        const auto directory = fs::relative(it->path().parent_path(), win32, error).generic_string();
        if (error) { error.clear(); continue; }
        directories_[directory].insert(*index);
    }
    for (const auto& [directory, archives] : directories_) byArchives[archives].push_back(directory);

    // Each install chunk in the manifest names its package directory, and
    // that is where its archives live -- whatever set of cas files a given
    // installation happens to hold. Keys are lower case, as mods' are.
    const auto* manifest = layout.field("installManifest");
    const auto* chunks = manifest ? manifest->field("installChunks") : nullptr;
    if (chunks) {
        for (const auto& entry : chunks->children) {
            const auto* name = entry.field("name");
            const auto* index = entry.field("persistentIndex");
            if (!name || name->type != 7 || name->text.empty() || !index || index->type != 8) continue;
            const auto value = index->payload();
            if (value.size() != 4) continue;
            const auto id = static_cast<std::uint32_t>(value[0]) | (static_cast<std::uint32_t>(value[1]) << 8) |
                (static_cast<std::uint32_t>(value[2]) << 16) | (static_cast<std::uint32_t>(value[3]) << 24);
            chunkDirectory_.emplace(id, lower(name->text));
        }
    }

    // Chunks the manifest does not name are matched by their archives.
    for (const auto* field : {"layeredInstallChunkFiles", "unlayeredInstallChunkFiles"}) {
        const auto* node = layout.field(field);
        if (!node || node->type != 19) continue;
        std::map<std::uint32_t, std::set<std::uint16_t>> declared;
        for_each_install_chunk_file(node->payload(), [&](std::uint32_t id, std::uint16_t archive) {
            declared[id].insert(archive);
        });
        // A chunk is only resolvable when one directory holds exactly the
        // archives it declares; the per-language folders all hold {1} and
        // stay ambiguous, which is fine because no mod references them.
        for (const auto& [id, archives] : declared) {
            const auto found = byArchives.find(archives);
            if (found != byArchives.end() && found->second.size() == 1)
                chunkDirectory_.emplace(id, lower(found->second.front()));
        }
    }
}

fb::BinaryBundle GameArchives::read_manifest(const fs::path& root, const fb::CasIdentifier& location,
                                             std::uint32_t offset, std::uint32_t size,
                                             const fs::path& gameRoot) const {
    const auto raw = read(root, location, offset, size);
    try {
        return fb::read_binary_bundle(raw);
    } catch (const std::exception&) {
        return fb::read_binary_bundle(fb::decode_cas(raw, {gameRoot}));
    }
}

std::vector<std::byte> GameArchives::read(const fs::path& root, const fb::CasIdentifier& location,
                                          std::uint32_t offset, std::uint32_t size) const {
    const auto path = root / "Win32" / fs::path(directory(location.installChunk)) /
        fs::path(archive_file(location.archive));
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open " + path_utf8(path));
    input.seekg(static_cast<std::streamoff>(offset));
    std::vector<std::byte> bytes(size);
    if (size && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("Cannot read " + path_utf8(path));
    return bytes;
}

std::string GameArchives::describe(const fb::CasIdentifier& location) const {
    const auto* directory = find_directory(location.installChunk);
    return (directory ? *directory : "chunk " + std::to_string(location.installChunk)) + "/" +
        archive_file(location.archive);
}

std::optional<std::uint64_t> GameArchives::archive_size(const fs::path& root, const fb::CasIdentifier& location) {
    const auto* directory = find_directory(location.installChunk);
    if (!directory) return std::nullopt;
    const auto path = root / "Win32" / fs::path(*directory) / fs::path(archive_file(location.archive));
    const auto known = sizes_.find(path.wstring());
    if (known != sizes_.end()) return known->second;
    std::error_code error;
    const auto size = fs::file_size(path, error);
    const auto result = error ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{size};
    sizes_.emplace(path.wstring(), result);
    return result;
}

const std::string* GameArchives::find_directory(std::uint32_t installChunk) const {
    const auto found = chunkDirectory_.find(installChunk);
    return found == chunkDirectory_.end() ? nullptr : &found->second;
}

const std::string& GameArchives::directory(std::uint32_t installChunk) const {
    const auto found = chunkDirectory_.find(installChunk);
    if (found == chunkDirectory_.end())
        throw std::runtime_error("No package directory matches install chunk " + std::to_string(installChunk));
    return found->second;
}

std::optional<std::uint16_t> GameArchives::archive_index(const std::string& stem) {
    const auto digits = stem.find_last_not_of("0123456789");
    if (digits == std::string::npos || digits + 1 >= stem.size()) return {};
    std::uint16_t value{};
    const auto end = stem.data() + stem.size();
    const auto [last, error] = std::from_chars(stem.data() + digits + 1, end, value); // range error past 65535, never throws
    if (error != std::errc{} || last != end) return {};
    return value;
}
}
