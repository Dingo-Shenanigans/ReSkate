#pragma once
#include "synchros_wire.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

// What a viewer keeps of the outfits it is sent (contrib/outfit-sharing.md, "Showing a received
// item"): the files arriving in pieces, and the cache they are kept in once whole.
namespace dingosdk::multiplayer::synchros {

// SHA-1 of a file as stored: how a file is named on the wire and in the cache, so a file two
// wearers' items share is fetched and kept once.
[[nodiscard]] ContentId content_id(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::string hex(const ContentId& id);

// One file arriving in Pieces. The wearer sends a file's pieces in order, each but the last
// max_piece long; anything else (a gap, an overlap, another total, too much, bytes that do not
// hash to the id it was asked for by) ends the file, and the exchange with it.
class Arriving final {
public:
    explicit Arriving(const File& expected);
    enum class Took : std::uint8_t { more, whole, refused };
    [[nodiscard]] Took take(const Piece& piece);
    // The whole file, once take() said so.
    [[nodiscard]] std::vector<std::uint8_t> release() { return std::move(bytes_); }
    [[nodiscard]] const ContentId& id() const noexcept { return id_; }
    [[nodiscard]] std::uint32_t received() const noexcept { return static_cast<std::uint32_t>(bytes_.size()); }

private:
    ContentId id_;
    std::uint32_t size_;
    std::vector<std::uint8_t> bytes_;
    bool done_{};
};

// The cache, under Mods/.synchros/cache (Engine/Vfs/synchros_outfit_mod.h): never listed as a mod.
//
//   files/<2 hex>/<40 hex>   each file, under its content id, written whole or not at all
//   wearers/<steamid>.bin    what that wearer was last seen wearing: its items' listings
//
// A file is kept while any wearer's listing names it, and the least recently used go first when
// the cache is over its limit. Forgetting a wearer (blocking them, emptying the cache) removes
// their listing and every file no other wearer's names.
class Cache final {
public:
    explicit Cache(std::filesystem::path root, std::uint64_t limit = 2ull << 30);

    [[nodiscard]] bool has(const ContentId& id) const;
    // Keeps a whole file. Its bytes must hash to `id`; false (and nothing written) when they do
    // not or the disk refuses.
    bool put(const ContentId& id, std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> read(const ContentId& id);

    // What a wearer has on, as the viewer was told, replacing what was kept for them.
    void remember(std::uint64_t wearer, const std::vector<ItemContents>& items);
    [[nodiscard]] std::vector<ItemContents> listing(std::uint64_t wearer) const;
    [[nodiscard]] std::vector<std::uint64_t> wearers() const;
    // The files of a wearer's items that are not kept yet: what to Fetch.
    [[nodiscard]] std::vector<ContentId> missing(std::uint64_t wearer) const;

    void forget(std::uint64_t wearer);
    void empty();

    [[nodiscard]] std::uint64_t size() const;
    [[nodiscard]] std::uint64_t limit() const noexcept { return limit_; }
    // Removes least recently used files until under the limit, never one named by `keep`'s
    // listings (the players in the session now).
    void trim(const std::set<std::uint64_t>& keep = {});

private:
    [[nodiscard]] std::filesystem::path file_path(const ContentId& id) const;
    [[nodiscard]] std::filesystem::path wearer_path(std::uint64_t wearer) const;
    [[nodiscard]] std::set<ContentId> named(const std::set<std::uint64_t>& wearers) const;
    void remove_unnamed();
    std::filesystem::path root_;
    std::uint64_t limit_;
};

} // namespace dingosdk::multiplayer::synchros
