#include "mod_merge_internal.h"
#include "Engine/Core/Platform/path_text.h"

#include <algorithm>
#include <cstring>
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

std::size_t CasStore::ContentHash::operator()(const Content& content) const noexcept {
    std::size_t value{};
    std::memcpy(&value, content.sha1.bytes.data(), sizeof(value));
    return value ^ content.size;
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
        if (placeholder(archive) || (archive_ && *archive_ != archive))
            throw std::logic_error("A waiting store writes to one archive index");
        archive_ = archive;
        auto& file = held_[path.wstring()];
        auto& held = file.bytes;
        // Hashed here, on the thread merging this superbundle, for settle to match against.
        const Content content{sha1_of(encoded), static_cast<std::uint32_t>(encoded.size())};
        if (const auto there = file.within.find(content); there != file.within.end())
            return {{true, installChunk, waiting_archive}, static_cast<std::uint32_t>(there->second), content.size};
        if (held.size() + encoded.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("The merged patch's own archive " + path_utf8(path) +
                                     " is full (4 GB); restart Skate so it is built again");
        const fb::BundleFileInfo info{{true, installChunk, waiting_archive}, static_cast<std::uint32_t>(held.size()),
                                      content.size};
        file.pieces.push_back({held.size(), content, 0});
        file.within.emplace(content, held.size());
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

fb::BundleFileInfo CasStore::carry(std::uint32_t installChunk, std::uint16_t archive, std::span<const std::byte> encoded) {
    if (!waiting_) throw std::logic_error("Only a waiting store carries payloads");
    return write(installChunk, archive, encoded);
}

void CasStore::settle(CasStore& waiting) {
    // In path order: which file comes first makes no difference to where anything lands.
    for (auto& [path, held] : waiting.held_) {
        // What the archive already holds is pointed at; the rest goes on its end in one append.
        auto& known = appended_[path];
        std::vector<std::byte> fresh;
        std::vector<std::pair<Piece*, std::uint64_t>> placed;   // a piece going on the end, where in `fresh`
        std::unordered_map<Content, std::uint64_t, ContentHash> pending;   // in `fresh` already
        for (auto& piece : held.pieces) {
            if (const auto there = known.find(piece.content); there != known.end()) {
                piece.settled = there->second;
                continue;
            }
            const auto [at, first] = pending.try_emplace(piece.content, fresh.size());
            if (first) {
                const auto* from = held.bytes.data() + piece.offset;
                fresh.insert(fresh.end(), from, from + piece.content.size);
            }
            placed.emplace_back(&piece, at->second);
        }
        if (!fresh.empty()) {
            const auto start = append(fs::path(path), fresh);
            for (auto& [piece, within] : placed) {
                piece->settled = start + within;
                known.try_emplace(piece->content, piece->settled);
            }
        }
        held.bytes = {};
    }
}

bool CasStore::settled(fb::CasIdentifier& location, std::uint32_t& offset) const {
    if (!placeholder(location)) return false;
    if (location.archive == carried_archive) throw std::logic_error("A placement names the reserved carried archive");
    const auto found = archive_ ? held_.find(archive_path(location.installChunk, *archive_).wstring()) : held_.end();
    if (found == held_.end()) throw std::logic_error("A placement waits in a store that holds nothing for it");
    // The piece the placement starts in (always at its start: a placement is one write).
    const auto& pieces = found->second.pieces;
    const auto after = std::upper_bound(pieces.begin(), pieces.end(), std::uint64_t{offset},
                                        [](std::uint64_t at, const Piece& piece) { return at < piece.offset; });
    if (after == pieces.begin()) throw std::logic_error("A placement waits before anything its store holds");
    const auto& piece = *std::prev(after);
    const auto moved = piece.settled + (offset - piece.offset);
    if (moved + piece.content.size > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("The merged patch's own archive " + path_utf8(fs::path(found->first)) +
                                 " is full (4 GB); restart Skate so it is built again");
    location.archive = *archive_;
    offset = static_cast<std::uint32_t>(moved);
    return true;
}

void CasStore::take(CasStore& part) {
    if (!waiting_ || !part.waiting_) throw std::logic_error("Only a waiting store takes another");
    if (part.archive_) {
        if (archive_ && *archive_ != *part.archive_) throw std::logic_error("A waiting store writes to one archive index");
        archive_ = part.archive_;
    }
    for (auto& [path, held] : part.held_) {
        auto& mine = held_[path].bytes;
        if (mine.size() + held.bytes.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::runtime_error("The merged patch's own archive " + path_utf8(fs::path(path)) +
                                     " is full (4 GB); restart Skate so it is built again");
        held.at = mine.size();
        auto& pieces = held_[path].pieces;
        auto& within = held_[path].within;
        for (const auto& piece : held.pieces) {
            pieces.push_back({piece.offset + held.at, piece.content, 0});
            within.try_emplace(piece.content, piece.offset + held.at);
        }
        mine.insert(mine.end(), held.bytes.begin(), held.bytes.end());
        held.bytes = {};
    }
}

bool CasStore::taken(fb::CasIdentifier& location, std::uint32_t& offset) const {
    if (!waits(location)) return false;
    const auto found = archive_ ? held_.find(archive_path(location.installChunk, *archive_).wstring()) : held_.end();
    if (found == held_.end()) throw std::logic_error("A placement waits in a store that holds nothing for it");
    offset = static_cast<std::uint32_t>(found->second.at + offset);   // (take checked it fits)
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
