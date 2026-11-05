#pragma once

// A character bundle-reference table (characters/customization/configs/
// cas_main_bundlereftable and its kind): which bundle to load for each preset,
// looked up by the FNV-1a hash of the preset's full path and of its leaf name.
// A cosmetic mod registers each preset it adds with a pair of rows pointing at
// a bundle the base already lists, so two mods' tables differ from the base
// only by the presets each added and can be combined exactly.

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::frostbite::bundle_ref {

// Resources named *_bundlereftable.
[[nodiscard]] bool is_table(std::string_view assetName);

struct Table {
    std::span<const std::byte> resource;
    std::span<const std::byte> resourceMeta;
};

struct MergedTable {
    std::vector<std::byte> resource;
    std::vector<std::byte> resourceMeta;
    std::size_t added{};     // presets taken from the edits
    std::size_t conflicts{}; // presets two edits put in different bundles; the first won
    // Presets added under their full path only, because another preset already
    // answers to the same leaf name (cust_tops/1_ap and cust_bottoms/1_ap).
    struct Shadowed {
        std::size_t edit{};  // index into `edits`
        std::string path;    // the preset left without a leaf lookup
        std::string holder;  // the preset its leaf name finds instead
    };
    std::vector<Shadowed> shadowed;
};

// Throws when a copy is not a table of this kind, or an edit changes the base's
// bundle list or existing rows (anything beyond added presets).
[[nodiscard]] MergedTable merge(const Table& base, std::span<const Table> edits);

// A preset and the bundle that loads it, by its index in the table's bundle list. The list is the
// game's own in every copy (an edit may not change it), so an index means the same bundle on any
// PC with this build: what Skate Synchros sends for a received item's presets.
struct Preset {
    std::string path;
    std::uint32_t bundle{};
    bool operator==(const Preset&) const = default;
};
// Every preset a table holds, path to bundle index. Throws when it is not a table of this kind.
[[nodiscard]] std::map<std::string, std::uint32_t> presets(const Table& table);
// `base` with `added` put in, as merge puts in an edit's presets: one the base already has where
// it is is skipped, one it has elsewhere is a conflict (the base's stays). Throws for a bundle index
// past the base's list.
[[nodiscard]] MergedTable with_presets(const Table& base, std::span<const Preset> added);

} // namespace dingosdk::frostbite::bundle_ref
