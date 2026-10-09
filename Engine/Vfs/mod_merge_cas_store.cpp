#include "mod_merge_internal.h"
#include "Engine/Core/Platform/path_text.h"

#include <fstream>
#include <limits>
#include <stdexcept>

namespace dingosdk::mods::detail {

bool unshift(const ArchivePlacement& placement, std::string_view directory, std::uint16_t& archive,
             std::uint32_t& offset) {
    const ArchivePlacement::Spot* block{};
    std::uint16_t origin{};
    for (const auto& [from, spot] : placement.at) {
        if (from.first != directory || spot.archive != archive || spot.offset > offset) continue;
        // Blocks are appended in archive order, so of two that start at the same
        // byte the earlier one is empty and the offset is in the later.
        if (!block || spot.offset >= block->offset) { block = &spot; origin = from.second; }
    }
    if (!block) return false;
    archive = origin;
    offset = static_cast<std::uint32_t>(offset - block->offset);
    return true;
}

CasStore::CasStore(fs::path baseRoot, fs::path output, const native_db::Node& layout)
    : GameArchives(std::move(baseRoot), layout), output_(std::move(output)) {
    keep_open();
}

fs::path CasStore::archive_path(std::uint32_t installChunk, std::uint16_t archive) const {
    return output_ / L"Win32" / fs::path(directory(installChunk)) / fs::path(archive_file(archive));
}

std::uint64_t CasStore::append(const fs::path& path, std::span<const std::byte> encoded) {
    auto& offset = offsets_[path.wstring()];
    if (!offset) {
        fs::create_directories(path.parent_path());
        forget(path); // unshare may put a private copy in its place
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
    const auto start = offset;
    offset += encoded.size();
    return start;
}

fb::BundleFileInfo CasStore::write(std::uint32_t installChunk, std::uint16_t archive,
                                   std::span<const std::byte> encoded) {
    const auto path = archive_path(installChunk, archive);
    if (waiting_) {
        // A placement that waits names no archive, so there can be only the one to settle into.
        if (archive == waiting_archive || (archive_ && *archive_ != archive))
            throw std::logic_error("A waiting store writes to one archive index");
        archive_ = archive;
        auto& held = held_[path.wstring()].bytes;
        if (held.size() + encoded.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("The merged patch's own archive " + path_utf8(path) +
                                     " is full (4 GB); restart Skate so it is built again");
        const fb::BundleFileInfo info{{true, installChunk, waiting_archive}, static_cast<std::uint32_t>(held.size()),
                                      static_cast<std::uint32_t>(encoded.size())};
        held.insert(held.end(), encoded.begin(), encoded.end());
        return info;
    }
    const auto start = append(path, encoded);
    return {{true, installChunk, archive}, static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(encoded.size())};
}

CasStore CasStore::waiting() const {
    auto copy = *this;
    copy.waiting_ = true;
    copy.archive_.reset();
    copy.held_.clear();
    copy.offsets_.clear();
    return copy;
}

void CasStore::settle(CasStore& waiting) {
    // In path order: which file comes first makes no difference to where anything lands.
    for (auto& [path, held] : waiting.held_) {
        held.at = append(fs::path(path), held.bytes);
        held.bytes = {};
    }
}

bool CasStore::settled(fb::CasIdentifier& location, std::uint32_t& offset) const {
    if (!waits(location)) return false;
    const auto found = archive_ ? held_.find(archive_path(location.installChunk, *archive_).wstring()) : held_.end();
    if (found == held_.end()) throw std::logic_error("A placement waits in a store that holds nothing for it");
    const auto moved = found->second.at + offset;
    if (moved > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("The merged patch's own archive " + path_utf8(fs::path(found->first)) +
                                 " is full (4 GB); restart Skate so it is built again");
    location.archive = *archive_;
    offset = static_cast<std::uint32_t>(moved);
    return true;
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
