#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

// An EBX's type-reference fixups that would crash the game's load. The fast-path partition load
// (Skate.exe 0x3def1c0) applies the other fixups first and then reads each type-reference slot as
// a type index; a type reference listed on a slot another fixup also writes (an import, a pointer,
// a resource reference, an instance's header, a boxed reference's pointer half), or listed twice,
// reads a pointer as an index and the game crashes (Skate.exe+0x3def285 / +0x3def262: Master
// Chief and TASM2 costume presets, 2026-10-17). The game's own files never do this. Removing those
// entries from the type-reference list leaves the slot to the fixup that owns it.
namespace dingosdk::frostbite::ebx {

struct FixupRepair {
    std::vector<std::byte> bytes;  // the document with those entries removed
    std::size_t removed{};
};

// The document repaired, or nothing when it needs none (or is not one this reads).
[[nodiscard]] std::optional<FixupRepair> repair_type_fixups(std::span<const std::byte> ebx);

} // namespace dingosdk::frostbite::ebx
