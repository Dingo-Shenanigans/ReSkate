#pragma once

// Moving instances between two EBX documents: shared by the generic merge
// (ebx_merge.cpp) and the material grid combiner (material_grid.cpp).

#include "ebx_document.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace dingosdk::frostbite::ebx::detail {

// Instance and type identity travel by name and guid, because the two documents
// number them independently.
class Carrier final {
public:
    Carrier(const Document& from, Document& to);

    // The instance `from` index resolves to the existing `to` index instead of
    // being carried, for a copy the caller has matched by content.
    void alias(std::size_t from, std::size_t to);

    [[nodiscard]] std::int32_t type(std::int32_t descriptor) const;
    // Brings an instance across, with everything it points at, once.
    std::size_t instance(std::size_t index);
    // A reference into another partition is an import: the same (file, class)
    // pair the target already lists is reused, anything new is added.
    std::int32_t import(std::int32_t index);
    // A type the source's EFIX list names, as the target's EFIX slot for it (added when missing):
    // boxed values and type references count in that list, and each document numbers its own.
    std::uint32_t fixup_slot(std::uint32_t index);
    [[nodiscard]] std::shared_ptr<Object> object(const Object& source);
    [[nodiscard]] Value value(const Value& source);
    // A box whose content was read, rebuilt in the target's numbering and marked to be written afresh.
    [[nodiscard]] BoxedReference boxed(const BoxedReference& source);

    std::size_t instances{};
    std::size_t renumbered{};

private:
    const Document& from_;
    Document& to_;
    std::map<std::string, std::int32_t> typeByName_;
    std::map<Guid, std::size_t> instanceByGuid_;
    std::map<std::size_t, std::size_t> carriedInternal_;
    std::map<std::size_t, std::size_t> aliases_;
    unsigned depth_{}; // instances being carried inside one another
};

// Instances hold their objects by shared pointer, so a copied Document still
// shares them; these give an edit its own copy.
[[nodiscard]] std::shared_ptr<Object> clone_object(const Object& source);
[[nodiscard]] Value clone_value(const Value& source);
// A document copy whose objects are its own.
[[nodiscard]] Document clone_document(const Document& source);

// Exported instances after the root, ascending by guid, internal ones last,
// with every internal pointer remapped (see ebx_merge.cpp).
void sort_instances(Document& document);

} // namespace dingosdk::frostbite::ebx::detail
