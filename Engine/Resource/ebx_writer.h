#pragma once

#include "ebx_document.h"

#include <cstdint>
#include <vector>

namespace dingosdk::frostbite::ebx {

[[nodiscard]] std::uint16_t ensure_fixup_type(Document& document, std::int32_t descriptor);

// A new array (one no record was read for) takes the hash of an array the document read for the
// same field of the same type. Without one it borrows another array's hash (the merge's way), or,
// with refuseNewArrayHashes (the map builder's), the write is refused: the RFL2 group table would
// need a new entry. A hash that is not a key of that table is refused the same way.
[[nodiscard]] std::vector<std::byte> write_document(const Document& document, bool refuseNewArrayHashes = false);

}
