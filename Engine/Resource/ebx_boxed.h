#pragma once

// What a boxed value holds, read field by field from the document's payload.
//
// A field of type 26 refers to a value kept in the payload's boxed section: a struct, an enum,
// a number, a text, a reference to an object, or a list of any of those (ReSkate Studio,
// docs/ebx.md, "Boxed values"; every one of the game's 456,560 boxed values is one of the
// kinds its table lists). The document keeps such a value as the bytes it was read as
// (BoxedValueRecord::rawBytes), so the merge reads it here as a flat list of leaves, each with
// the place in the payload it was read from, and changes a value by writing over those bytes.

#include "ebx_document.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dingosdk::frostbite::ebx {

struct BoxedLeaf {
    enum class Kind : std::uint8_t {
        shape,    // what holds the leaves after it: a boxed value's kind, a struct's type, a list's length
        bytes,    // a number, a switch, a guid, an enum, a resource id: its bytes as stored
        text,     // a text: `offset` is its slot, which holds the distance to the string
        pointer,  // a reference to an object: `offset` is its slot
        type      // a type reference or delegate, by the type it names
    };
    Kind kind{};
    std::uint32_t offset{};        // in the payload
    std::vector<std::byte> bytes;  // Kind::bytes
    std::string text;              // Kind::shape, Kind::text, Kind::type
    PointerReference pointer;      // Kind::pointer
};

// The leaves of the boxed value `reference` refers to, in a fixed order (a struct's fields in the
// order of its type's fields, a base's first; a list's entries in order), so two values of one
// shape give leaves that line up. Empty when the value cannot be read: no record at that offset,
// a type the document does not describe, an offset outside the payload.
[[nodiscard]] std::optional<std::vector<BoxedLeaf>> read_boxed(const Document& document,
                                                                const BoxedReference& reference);

// Writes `bytes` over the payload at `offset` and over the copy the boxed value there keeps,
// so the writer, which lays boxed values down from their own bytes and carries what they point
// at from the payload, writes the new value.
void write_boxed_bytes(Document& document, std::uint32_t offset, std::span<const std::byte> bytes);

// Points the text slot at `slot` at `text`: the string is added past the end of the payload,
// where the writer finds it as it finds the document's own strings.
void write_boxed_text(Document& document, std::uint32_t slot, const std::string& text);

// Brings a boxed value from `from` into `to`, for an instance carried from one document to the
// other. Only a value that points at nothing (no text, reference, list or boxed value in it) is
// carried: its bytes are the whole of it. `typeRow` gives the row of `to`'s fix-up type list that
// names the type `from`'s row names, or nothing. Throws when the value cannot be carried.
[[nodiscard]] BoxedReference carry_boxed(const Document& from, const BoxedReference& reference, Document& to,
                                         const std::function<std::optional<std::uint16_t>(std::uint16_t)>& typeRow);

} // namespace dingosdk::frostbite::ebx
