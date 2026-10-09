#pragma once
// Read access to a data layer's cas archives: <root>/Win32/<package>/cas_NN.cas,
// with each install chunk's package directory taken from layout.toc.
#include "native_db.h"
#include "Engine/Resource/binary_bundle.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace dingosdk::vfs {
// "cas_NN.cas"
std::string archive_file(std::uint16_t archive);
// Each record of layout.toc's install chunk file tables: a little-endian
// archive index followed by the install chunk id it belongs to.
template<class Visit>
void for_each_install_chunk_file(std::span<const unsigned char> table, Visit&& visit) {
    for (std::size_t at = 0; at + 8 <= table.size(); at += 8) {
        const auto archive = static_cast<std::uint16_t>(table[at] | (table[at + 1] << 8));
        std::uint32_t id{};
        for (int byte = 3; byte >= 0; --byte)
            id = (id << 8) | static_cast<std::uint32_t>(table[at + 2 + byte]);
        visit(id, archive);
    }
}
// A parsed layout.toc. The tree's byte arrays are views into `buffer`, so the
// two travel together (moving a vector keeps its storage).
struct Layout {
    std::vector<unsigned char> buffer;
    native_db::Node root;
};
// Parses a layout.toc file, envelope included.
Layout read_layout(const std::filesystem::path& path);

class GameArchives {
public:
    // `root` is a data layer's directory (the game's Data folder); its layout
    // names the package directory of every install chunk.
    GameArchives(std::filesystem::path root, const native_db::Node& layout);

    [[nodiscard]] std::vector<std::byte> read(const std::filesystem::path& root, const frostbite::CasIdentifier& location,
                                              std::uint32_t offset, std::uint32_t size) const;
    // The bundle manifest is stored uncompressed, unlike the asset payloads
    // around it, so raw comes first and the block decoder is the fallback.
    [[nodiscard]] frostbite::BinaryBundle read_manifest(const std::filesystem::path& root,
        const frostbite::CasIdentifier& location, std::uint32_t offset, std::uint32_t size,
        const std::filesystem::path& gameRoot) const;

    // "<package>/cas_NN.cas" for a log line; never throws for an unknown chunk.
    [[nodiscard]] std::string describe(const frostbite::CasIdentifier& location) const;
    // The archive's size on disk under `root`, remembered per file; nullopt
    // when the file is not there or its package directory is unknown.
    [[nodiscard]] std::optional<std::uint64_t> archive_size(const std::filesystem::path& root,
                                                            const frostbite::CasIdentifier& location);
    [[nodiscard]] const std::string* find_directory(std::uint32_t installChunk) const;
    [[nodiscard]] const std::string& directory(std::uint32_t installChunk) const;
    // Every install chunk whose archives live in `directory`.
    [[nodiscard]] std::vector<std::uint32_t> chunks_in(const std::string& directory) const;
    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    static std::optional<std::uint16_t> archive_index(const std::string& stem);
    // From here on every archive read stays open until this object goes: for a merge, which
    // reads tens of thousands of payloads out of a few hundred files. Opening a file costs
    // far more than reading from it (the PC's anti-virus checks every open and close).
    // Reads may then come from any thread.
    void keep_open();
    // Closes an archive kept open, before the file is replaced by another.
    void forget(const std::filesystem::path& file) const;

private:
    struct Open; // the archives kept open (keep_open); null: each read opens its file
    std::shared_ptr<Open> open_;
    std::filesystem::path root_;
    std::map<std::string, std::set<std::uint16_t>> directories_;
    std::map<std::uint32_t, std::string> chunkDirectory_;
    std::map<std::wstring, std::optional<std::uint64_t>> sizes_;
};
}
