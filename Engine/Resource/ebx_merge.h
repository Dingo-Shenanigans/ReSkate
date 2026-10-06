#pragma once

#include "ebx_document.h"

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

namespace dingosdk::frostbite::ebx {

struct MergeSummary {
    std::size_t instances{};   // instances taken from the edits
    std::size_t arrayEntries{};// root array entries taken from the edits
    std::size_t renumbered{};  // carried instances whose object id was re-derived
    // Root array entries left behind because `carry` turned their target down.
    struct Dropped {
        std::size_t edit{};    // index into `edits`
        ImportReference target;
    };
    std::vector<Dropped> dropped;
    std::size_t repeated{};    // appended references an earlier entry had already added, left out
    std::size_t values{};      // plain values an edit changed in place, taken from it
    std::size_t contested{};   // of those, values an earlier edit had changed to something else
    // Edits (indices into `edits`) with a change that did not come across: a reference
    // pointed somewhere else, a list resized anywhere but at the root's end, an instance gone.
    std::vector<std::size_t> uncarried;
    // Edits whose in-place changes could not all be looked for, because their internal
    // instances do not line up with the base's or the document holds boxed values.
    std::vector<std::size_t> unread;

    // Every edit's in-place changes are in the result, as far as anyone can tell.
    [[nodiscard]] bool exact() const noexcept { return uncarried.empty() && unread.empty(); }
};

// Whether a root array entry an edit appended comes across, asked for an entry
// that is a reference into another document. Such a reference only resolves
// while the document it names is loaded; one that is not loads as a null object.
using CarryImport = std::function<bool(const ImportReference&)>;

// Combines several independently edited copies of one EBX asset with the base
// they were all derived from. Each edit contributes the instances it added, the
// entries it appended to the root's arrays (a reference two edits both append is
// listed once), and the plain values it changed in place: a number, a string, a
// guid. Two edits to one value cannot both be honoured, so edits are applied in
// the order given and the last one wins. A change of any other shape (a
// reference pointed elsewhere, a list resized in the middle of the document)
// stays as the base had it, and the summary names the edit it was in.
[[nodiscard]] Document merge_documents(const Document& base,
                                       std::span<const Document* const> edits,
                                       MergeSummary* summary = nullptr,
                                       const CarryImport& carry = {});

// Takes out the root array entries that refer to a document `keep` turns down,
// and the imports nothing refers to afterwards. Returns how many entries went.
std::size_t drop_root_references(Document& document, const CarryImport& keep);

} // namespace dingosdk::frostbite::ebx
