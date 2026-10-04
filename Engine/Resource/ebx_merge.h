#pragma once

#include "ebx_document.h"

#include <cstddef>
#include <span>
#include <vector>

namespace dingosdk::frostbite::ebx {

struct MergeSummary {
    std::size_t instances{};       // instances taken from the edits
    std::size_t arrayEntries{};    // root array entries taken from the edits
    std::size_t renumbered{};      // carried instances whose object id was re-derived
    std::size_t rootReplacements{};// edits whose changed root fields were honoured
};

// Combines several independently edited copies of one EBX asset with the base
// they were all derived from. Each edit contributes the instances it added and
// the entries it appended to the root's arrays; anything an edit merely changed
// in place stays as the base had it, because two edits to one value cannot both
// be honoured — unless a single edit is given on its own. One edit cannot
// disagree with another, so its root replaces the base root outright and its
// in-place changes (a skeleton's bone poses, an object's transforms, ...) take
// effect. Edits are applied in the order given, so the last one wins a tie.
[[nodiscard]] Document merge_documents(const Document& base,
                                       std::span<const Document* const> edits,
                                       MergeSummary* summary = nullptr);

} // namespace dingosdk::frostbite::ebx
