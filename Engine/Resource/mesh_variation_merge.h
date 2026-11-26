#pragma once

// A level bundle's mesh variation database (a MeshVariationDatabase document, named
// "<bundle>/meshvariationdb_win32") says, for each mesh the bundle draws, which materials of it
// draw with which shader. The game registers its entries by the mesh's name hash and the entry's
// two variation hashes (Engine/Game/Build/20260929/mesh_variations.h), and a mesh without an entry
// is drawn invisible. A bundle holds one asset per name, so every prop mod adds a database under
// the same name, each with its own props' entries, and kept whole, only one mod's props showed
// (a boot of 280 mods, 2026-11-19).
//
// The combined database holds every copy's entries. Where two give an entry for the same mesh and
// variation, a copy that changes the game's entry wins over the game, and of two mods the higher
// one in the load order wins; the instances an entry points at (its material variations, its
// radiosity materials) come with it.

#include "ebx_document.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk::frostbite::ebx {

// Whether an asset is named as a level bundle's mesh variation database.
[[nodiscard]] bool variation_database_name(std::string_view assetName) noexcept;
// Whether a document is one.
[[nodiscard]] bool variation_database(const Document& document) noexcept;

struct VariationMerge {
    Document document;
    std::size_t entries{};   // entries in the combined database
    std::size_t added{};     // entries brought in from copies other than the one it started from
    std::size_t changed{};   // entries of the game's that a copy changed, taken from it
    // Two copies give the same mesh and variation different entries: the higher copy's is kept.
    struct Clash {
        std::size_t kept{};  // indices into `copies`
        std::size_t lost{};
        std::string mesh;    // the mesh's document guid
    };
    std::vector<Clash> clashes;
    // Copies whose entries could not be brought across (a type the combined database does not
    // describe, say), with why; the database is as if they had not been given.
    struct Left {
        std::size_t copy{};
        std::string why;
    };
    std::vector<Left> left;
};

// `game` is the game's own copy, or null for a database only mods add; `copies` are the mods',
// lowest priority first. Throws when a document is not a mesh variation database.
[[nodiscard]] VariationMerge merge_variation_databases(const Document* game,
                                                       std::span<const Document* const> copies);

} // namespace dingosdk::frostbite::ebx
