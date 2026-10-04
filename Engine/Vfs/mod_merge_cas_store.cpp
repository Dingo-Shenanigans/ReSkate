#include "mod_merge_internal.h"
#include "Engine/Core/Platform/path_text.h"

#include <fstream>
#include <limits>
#include <stdexcept>

namespace dingosdk::mods::detail {

bool unshift(const ArchivePlacement& placement, std::string_view directory, std::uint16_t& archive,
             std::uint32_t& offset) {
    const ArchivePlacement::Spot* best{};
    std::uint16_t origin{};
    for (const auto& [key, spot] : placement.at) {
        if (key.first != directory || spot.archive != archive || spot.offset > offset) continue;
        // When two blocks start at the same offset, the earlier block is empty. The merge appends
        // archives in name order, which is index order for zero-padded names, so use the later block.
        if (!best || spot.offset >= best->offset) { best = &spot; origin = key.second; }
    }
    if (!best) return false;
    archive = origin;
    offset = static_cast<std::uint32_t>(offset - best->offset);
    return true;
}

ReadBack read_back(const fs::path& contributor, const fs::path& output, fb::BundleFileInfo file, bool written,
                   const ArchivePlacement* placement, const std::string* directory) {
    if (written) return {output, file};
    if (placement && directory && file.location.patch)
        unshift(*placement, *directory, file.location.archive, file.offset);
    return {contributor, file};
}

CasStore::CasStore(fs::path baseRoot, fs::path output, const native_db::Node& layout)
    : GameArchives(std::move(baseRoot), layout), output_(std::move(output)) {}

fb::BundleFileInfo CasStore::write(std::uint32_t installChunk, std::uint16_t archive,
                                   std::span<const std::byte> encoded) {
    const auto path = output_ / L"Win32" / fs::path(directory(installChunk)) /
        fs::path(archive_file(archive));
    auto& offset = offsets_[path.wstring()];
    if (!offset) {
        fs::create_directories(path.parent_path());
        unshare(path);
        std::error_code error;
        const auto existing = fs::file_size(path, error);
        if (!error) offset = existing;
    }
    // A placement holds a 32-bit offset; one past it would wrap round and
    // point the game at some other payload.
    if (offset + encoded.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("The merged patch's own archive " + path_utf8(path) +
                                 " is full (4 GB); restart Skate so it is built again");
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out || !out.write(reinterpret_cast<const char*>(encoded.data()),
                           static_cast<std::streamsize>(encoded.size())))
        throw std::runtime_error("Cannot append to " + path_utf8(path));
    fb::BundleFileInfo info{{true, installChunk, archive}, static_cast<std::uint32_t>(offset),
                            static_cast<std::uint32_t>(encoded.size())};
    offset += encoded.size();
    return info;
}

void CasStore::shift(fb::CasIdentifier& location, std::uint32_t& offset,
                     const ArchivePlacement* placement) const {
    if (!location.patch || !placement) return;
    const auto found = placement->at.find({directory(location.installChunk), location.archive});
    if (found == placement->at.end()) return;
    const auto moved = static_cast<std::uint64_t>(offset) + found->second.offset;
    if (moved > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Combined mod archives exceed 4 GB, which cas offsets cannot address");
    location.archive = found->second.archive;
    offset = static_cast<std::uint32_t>(moved);
}

} // namespace dingosdk::mods::detail
